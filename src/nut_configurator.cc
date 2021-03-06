/*  =========================================================================
    nut_configurator - NUT configurator class

    Copyright (C) 2014 - 2016 Eaton

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
    =========================================================================
*/

/*
@header
    nut_configurator - NUT configurator class
@discuss
@end
*/

#include "nut_configurator.h"
#include <fty_common_mlm_subprocess.h>
#include <fty_common_filesystem.h>
#include "nutscan.h"
#include <fty_log.h>
#include "cidr.h"

#include <cxxtools/jsondeserializer.h>
#include <cxxtools/regex.h>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <czmq.h>
#include <string>

using namespace shared;

#define NUT_PART_STORE "/var/lib/fty/fty-nut/devices"

static const char * NUTConfigXMLPattern = "[[:blank:]]driver[[:blank:]]+=[[:blank:]]+\"netxml-ups\"";
/* TODO: This explicitly lists NUT MIB mappings for the static snmp-ups driver,
 * and only for Eaton devices, as it seems...
 * As we integrate DMF support, consider also/instead using mapping names from
 * there, if applicable.
 */
static const char * NUTConfigEpduPattern = "[[:blank:]](mibs[[:blank:]]+=[[:blank:]]+\"(eaton_epdu|aphel_genesisII|aphel_revelation|pulizzi_switched1|pulizzi_switched2|emerson_avocent_pdu)\"|"
                                           "desc[[:blank:]]+=[[:blank:]]+\"[^\"]+ epdu [^\"]+\")";
static const char * NUTConfigCanSnmpPattern = "[[:blank:]]driver[[:blank:]]+=[[:blank:]]+\"snmp-ups(-old|-dmf)?\"";

static const char * NUTConfigATSPattern = "[[:blank:]]mibs[[:blank:]]*=[[:blank:]]*\"[^\"]*ats[^\"]*\"";

std::vector<std::string>::const_iterator NUTConfigurator::stringMatch(const std::vector<std::string> &texts, const char *pattern) {
    log_debug("regex: %s", pattern );
    cxxtools::Regex reg( pattern, REG_EXTENDED | REG_ICASE );
    for( auto it = texts.begin(); it != texts.end(); ++it ) {
        if( reg.match( *it ) ) {
            log_debug("regex: match found");
            return it;
        }
    }
    log_debug("regex: not found");
    return texts.end();
}


bool NUTConfigurator::match( const std::vector<std::string> &texts, const char *pattern) {
    return stringMatch(texts,pattern) != texts.end();
}

bool NUTConfigurator::isEpdu( const std::vector<std::string> &texts) {
    return match( texts, NUTConfigEpduPattern );
}

bool NUTConfigurator::isAts( const std::vector<std::string> &texts) {
    return match( texts, NUTConfigATSPattern );
}

bool NUTConfigurator::isUps( const std::vector<std::string> &texts) {
    return ! (isEpdu(texts) || isAts (texts));
}

bool NUTConfigurator::canSnmp( const std::vector<std::string> &texts) {
    return match( texts, NUTConfigCanSnmpPattern );
}

bool NUTConfigurator::canXml( const std::vector<std::string> &texts) {
    return match( texts, NUTConfigXMLPattern );
}

std::vector<std::string>::const_iterator NUTConfigurator::getBestSnmpMib(const std::vector<std::string> &configs) {
    static const std::vector<std::string> snmpMibPriority = {
        "pw", "mge", ".+"
    };
    for( auto mib = snmpMibPriority.begin(); mib != snmpMibPriority.end(); ++mib ) {
        std::string pattern = ".+[[:blank:]]mibs[[:blank:]]+=[[:blank:]]+\"" + *mib + "\"";
        auto it = stringMatch( configs, pattern.c_str() );
        if( it != configs.end() ) return it;
    }
    return configs.end();
}

std::vector<std::string>::const_iterator NUTConfigurator::selectBest(const std::vector<std::string> &configs) {
    // don't do any complicated decision on empty/single set
    if( configs.size() <= 1 ) return configs.begin();

    log_debug("isEpdu: %i; isUps: %i; isAts: %i; canSnmp: %i; canXml: %i", isEpdu(configs), isUps(configs), isAts(configs), canSnmp(configs), canXml(configs) );
    if( canSnmp( configs ) && ( isEpdu( configs ) || isAts( configs ) ) ) {
        log_debug("SNMP capable EPDU => Use SNMP");
        return getBestSnmpMib( configs );
    } else {
        if( canXml( configs ) ) {
            log_debug("XML capable device => Use XML");
            return stringMatch( configs, NUTConfigXMLPattern );
        } else {
            log_debug("SNMP capable device => Use SNMP");
            return getBestSnmpMib( configs );
        }
    }
}

void NUTConfigurator::systemctl( const std::string &operation, const std::string &service )
{
    systemctl(operation, &service, &service + 1);
}

template<typename It>
void NUTConfigurator::systemctl( const std::string &operation, It first, It last)
{
    if (first == last)
        return;
    std::vector<std::string> _argv = {"sudo", "systemctl", operation };
    // FIXME: Split the argument list into chunks if its size is close to
    // sysconf(_SC_ARG_MAX). Note that the limit is reasonably high on modern
    // kernels (stack size / 4, i.e. 2MB typically), so we will only hit it
    // with with five digit device counts.
    _argv.insert(_argv.end(), first, last);
    MlmSubprocess::SubProcess systemd( _argv );
    if( systemd.run() ) {
        int result = systemd.wait();
        log_info("sudo systemctl %s result %i (%s) for following units",
                 operation.c_str(),
                 result,
                 (result == 0 ? "ok" : "failed"));
        for (It it = first; it != last; ++it)
            log_info(" - %s", it->c_str());
    } else {
        log_error("can't run sudo systemctl %s for following units",
                  operation.c_str());
        for (It it = first; it != last; ++it)
            log_error(" - %s", it->c_str());
    }
}

void NUTConfigurator::updateNUTConfig() {
    // Run the helper script
    std::vector<std::string> _argv = { "sudo", "fty-nutconfig" };
    MlmSubprocess::SubProcess systemd( _argv );
    if( systemd.run() ) {
        int result = systemd.wait();
        log_info("sudo fty-nutconfig %i (%s)",
                 result,
                 (result == 0 ? "ok" : "failed"));
    } else {
        log_error("can't run sudo fty-nutconfig command");
    }
}

// compute hash (sha-1) of a file
static char*
s_digest (const char* file)
{
    assert (file);
    zdigest_t *digest = zdigest_new ();

    int fd = open (file, O_NOFOLLOW | O_RDONLY);
    if (fd == -1) {
        log_info ("Cannot open file '%s', digest won't be computed: %s", file, strerror (errno));
        return NULL;
    }
    std::string buffer = MlmSubprocess::read_all (fd);
    close (fd);

    zdigest_update (digest, (byte*) buffer.c_str (), buffer.size ());
    char *ret = strdup (zdigest_string (digest));
    zdigest_destroy (&digest);
    return ret;
}

// compute hash (sha-1) of a std::stringstream
static char*
s_digest (const std::stringstream& s)
{
    zdigest_t *digest = zdigest_new ();
    zdigest_update (digest, (byte*) s.str ().c_str (), s.str ().size ());
    char *ret = strdup (zdigest_string (digest));
    zdigest_destroy (&digest);
    return ret;
}

bool NUTConfigurator::configure( const std::string &name, const AutoConfigurationInfo &info ) {
    log_debug("NUT configurator created");

    // get polling interval first
    std::string polling = "30";
    {
        zconfig_t *config = zconfig_load ("/etc/fty-nut/fty-nut.cfg");
        if (config) {
            polling = zconfig_get (config, "nut/polling_interval", "30");
            zconfig_destroy (&config);
        }
    }

    std::vector<std::string> configs;

    std::string IP = "127.0.0.1"; // Fake value for local-media devices or dummy-upses, either passed with an upsconf_block
        // TODO: (lib)nutscan supports local media like serial or USB,
        // as well as other remote protocols like IPMI. Use them later.
    if(info.asset->have_upsconf_block()) {
        // TODO: Refactor to optimize string manipulations
        std::string UBA = info.asset->upsconf_block(); // UpsconfBlockAsset - as stored in contents of the asset
        char SEP = UBA.at(0);
        if ( SEP == '\0' || UBA.at(1) == '\0' ) {
            log_info("device %s is configured with an empty explicit upsconf_block from its asset (adding asset name as NUT device-tag with no config)",
                name.c_str());
            configs = { "[" + name + "]\n\n" };
        } else {
            // First character of the sufficiently long UB string
            // defines the user-selected line separator character
            std::string UBN = UBA.substr(1); //UpsconfBlockNut - with EOL chars, without leading SEP character
            std::replace( UBN.begin(), UBN.end(), SEP, '\n' );
            if ( UBN.at(0) == '[' ) {
                log_info("device %s is configured with a complete explicit upsconf_block from its asset: \"%s\" including a custom NUT device-tag",
                    name.c_str(), UBN.c_str());
                configs = { UBN + "\n" };
            } else {
                log_info("device %s is configured with a content-only explicit upsconf_block from its asset: \"%s\" (prepending asset name as NUT device-tag)",
                    name.c_str(), UBN.c_str());
                configs = { "[" + name + "]\n" + UBN + "\n" };
            }
        }
    } else {
        if (info.asset->IP().empty()) {
            log_error("device %s has no IP address", name.c_str() );
            return true;
        }
        IP = info.asset->IP();

        std::vector <std::string> communities;
        zconfig_t *config = zconfig_load ("/etc/default/fty.cfg");
        if (config) {
            zconfig_t *item = zconfig_locate (config, "snmp/community");
            if (item) {
                bool is_array = false;
                zconfig_t *child = zconfig_child (item);
                while (child) {
                    if (!streq (zconfig_value (child), "")) {
                        is_array = true;
                        communities.push_back (zconfig_value (child));
                    }
                    child = zconfig_next (child);
                }
                if (!is_array && !streq (zconfig_value (item), ""))
                    communities.push_back (zconfig_value (item));
            }
            zconfig_destroy (&config);
        }
        else {
            log_warning ("Config file '%s' could not be read.", "/etc/default/fty.cfg");
        }
        communities.push_back ("public");

        bool use_dmf = info.asset->upsconf_enable_dmf();
        for (const auto& c : communities) {
            log_debug("Trying community == %s", c.c_str());
            if (nut_scan_snmp (name, CIDRAddress (IP), c, use_dmf, configs) == 0 && !configs.empty ()) {
                break;
            }
        }
        nut_scan_xml_http (name, CIDRAddress(IP), configs);
    }

    auto it = selectBest( configs );
    if( it == configs.end() ) {
        log_error("nut-scanner failed for device \"%s\" at IP address \"%s\", no suitable configuration found",
            name.c_str(), IP.c_str() );
        return false; // try again later
    }
    std::string deviceDir = NUT_PART_STORE;
    mkdir_if_needed( deviceDir.c_str() );
    std::stringstream cfg;

    std::string config_name = std::string(NUT_PART_STORE) + path_separator() + name;
    char* digest_old = s_digest (config_name.c_str ());
    cfg << *it;
    {
        std::string s = *it;
        // prototypes expects std::vector <std::string> - lets create fake vector
        // this is not performance critical code anyway
        std::vector <std::string> foo = {s};
        if (isEpdu (foo) && canSnmp (foo)) {
            log_debug ("add synchronous = yes");
            cfg << "\tsynchronous = yes\n";
        }
        if (canXml (foo)) {
            log_debug ("add timeout for XML driver");
            cfg << "\ttimeout = 15\n";
        }
        log_debug ("add polling for driver");
        if (canSnmp (foo)) {
            cfg << "\tpollfreq = " << polling << "\n";
        } else {
            cfg << "\tpollinterval = " << polling << "\n";
        }
    }
    char* digest_new = s_digest (cfg);

    log_debug ("%s: digest_old=%s, digest_new=%s", config_name.c_str (), digest_old ? digest_old : "(null)", digest_new);
    if (!digest_old || !streq (digest_old, digest_new)) {
        std::ofstream cfgFile;
        cfgFile.open (config_name);
        cfgFile << cfg.str ();
        cfgFile.flush ();
        cfgFile.close ();
        log_info("creating new config file %s/%s", NUT_PART_STORE, name.c_str() );
        start_drivers_.insert("nut-driver@" + name);
    }
    zstr_free (&digest_new);
    zstr_free (&digest_old);
    return true;
}

void NUTConfigurator::erase(const std::string &name)
{
    log_info("removing configuration file %s/%s", NUT_PART_STORE, name.c_str());
    std::string fileName = std::string(NUT_PART_STORE)
        + path_separator()
        + name;
    remove( fileName.c_str() );
    stop_drivers_.insert("nut-driver@" + name);
}

void NUTConfigurator::commit()
{
    systemctl("disable", stop_drivers_.begin(),  stop_drivers_.end());
    systemctl("stop",    stop_drivers_.begin(),  stop_drivers_.end());
    updateNUTConfig();
    systemctl("restart", start_drivers_.begin(), start_drivers_.end());
    systemctl("enable",  start_drivers_.begin(), start_drivers_.end());
    if (!stop_drivers_.empty() || !start_drivers_.empty())
        systemctl("reload-or-restart", "nut-server");
    stop_drivers_.clear();
    start_drivers_.clear();
}

bool NUTConfigurator::known_assets(std::vector<std::string>& assets)
{
    return shared::is_file_in_directory(NUT_PART_STORE, assets);
}

void
nut_configurator_test (bool verbose)
{
}
