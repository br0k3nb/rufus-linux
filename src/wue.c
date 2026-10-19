/*
 * Rufus: The Reliable USB Formatting Utility
 * Windows User Experience
 * Copyright © 2022-2026 Pete Batard <pete@akeo.ie>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Rufus Linux port notes
 * ----------------------
 * Port of upstream wue.c. The answer file (unattend.xml) creation, WinPE
 * setup, WIM index/version parsing and the boot.wim customization are
 * upstream's code; the Linux specific changes are:
 * - targets are identified by their mount point ('drive_name') instead of a
 *   drive letter, and paths use '/';
 * - the host locale/timezone/keyboard that upstream reads from the Windows
 *   registry/NLS APIs come from the Linux locale, timezone and XKB settings;
 * - the TPM/Secure Boot/RAM bypass keys are always set through unattend.xml,
 *   which is upstream's own fallback when it can't edit the offline registry
 *   hive of boot.wim (Linux has no offline registry API);
 * - Windows To Go needs the Windows bcdboot/bcdedit tools to create the BCD
 *   store of the target, which have no Linux equivalent: it is reported as
 *   unsupported (SetWinToGoIndex() returns -1 and SetupWinToGo() fails).
 */


#include "rfus.h"
#include "localization.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>

#include "xml.h"
#include "wimlib.h"
#include "wimlib_port.h"

 /* Memory leaks detection - define _CRTDBG_MAP_ALLOC as preprocessor macro */
#ifdef _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

const char* bypass_name[] = { "BypassTPMCheck", "BypassSecureBootCheck", "BypassRAMCheck" };

int unattend_xml_flags = 0, wintogo_index = -1, wininst_index = 0;
int unattend_xml_mask = UNATTEND_DEFAULT_SELECTION_MASK;
int unattend_edition_index = 1;
char *unattend_xml_path = NULL, unattend_username[MAX_USERNAME_LENGTH];
uint32_t removable_section[2] = { 0, 0 };

extern BOOL validate_md5sum;
extern uint64_t md5sum_totalbytes;
extern StrArray modified_files;
extern const char* efi_archname[ARCH_MAX];

/* ------------------------------------------------------------------ */
/* PORT: host settings that upstream reads from Windows APIs           */
/* ------------------------------------------------------------------ */

/* Windows locale name ("en-US") for the current Linux locale ("en_US.UTF-8") */
static const char* GetLocaleName(void)
{
	static char name[32];
	const char* env[] = { "LC_ALL", "LC_MESSAGES", "LANG" };
	const char* loc = NULL;
	size_t i;

	for (i = 0; i < ARRAYSIZE(env) && (loc == NULL || loc[0] == 0); i++)
		loc = getenv(env[i]);
	if (loc == NULL || loc[0] == 0 || loc[0] == 'C' || strcmp(loc, "POSIX") == 0)
		return "en-US";
	static_strcpy(name, loc);
	name[strcspn(name, ".@")] = 0;
	for (i = 0; name[i] != 0; i++)
		if (name[i] == '_')
			name[i] = '-';
	return name;
}

/* Windows time zone name for the host's IANA time zone (from the CLDR
 * windowsZones mapping, for the zones a Windows install would offer) */
static const char* GetWindowsTimeZone(void)
{
	static const struct { const char* iana; const char* windows; } tz_map[] = {
		{ "Etc/UTC", "UTC" }, { "UTC", "UTC" }, { "Etc/GMT", "UTC" }, { "Universal", "UTC" },
		{ "Europe/London", "GMT Standard Time" }, { "Europe/Dublin", "GMT Standard Time" },
		{ "Europe/Lisbon", "GMT Standard Time" }, { "Atlantic/Canary", "GMT Standard Time" },
		{ "Atlantic/Reykjavik", "Greenwich Standard Time" }, { "Africa/Abidjan", "Greenwich Standard Time" },
		{ "Europe/Berlin", "W. Europe Standard Time" }, { "Europe/Amsterdam", "W. Europe Standard Time" },
		{ "Europe/Rome", "W. Europe Standard Time" }, { "Europe/Stockholm", "W. Europe Standard Time" },
		{ "Europe/Vienna", "W. Europe Standard Time" }, { "Europe/Zurich", "W. Europe Standard Time" },
		{ "Europe/Oslo", "W. Europe Standard Time" }, { "Europe/Luxembourg", "W. Europe Standard Time" },
		{ "Europe/Paris", "Romance Standard Time" }, { "Europe/Brussels", "Romance Standard Time" },
		{ "Europe/Madrid", "Romance Standard Time" }, { "Europe/Copenhagen", "Romance Standard Time" },
		{ "Europe/Warsaw", "Central European Standard Time" }, { "Europe/Zagreb", "Central European Standard Time" },
		{ "Europe/Sarajevo", "Central European Standard Time" }, { "Europe/Skopje", "Central European Standard Time" },
		{ "Europe/Budapest", "Central Europe Standard Time" }, { "Europe/Prague", "Central Europe Standard Time" },
		{ "Europe/Belgrade", "Central Europe Standard Time" }, { "Europe/Bratislava", "Central Europe Standard Time" },
		{ "Europe/Ljubljana", "Central Europe Standard Time" }, { "Africa/Lagos", "W. Central Africa Standard Time" },
		{ "Europe/Athens", "GTB Standard Time" }, { "Europe/Bucharest", "GTB Standard Time" },
		{ "Europe/Helsinki", "FLE Standard Time" }, { "Europe/Kiev", "FLE Standard Time" },
		{ "Europe/Kyiv", "FLE Standard Time" }, { "Europe/Riga", "FLE Standard Time" },
		{ "Europe/Sofia", "FLE Standard Time" }, { "Europe/Tallinn", "FLE Standard Time" },
		{ "Europe/Vilnius", "FLE Standard Time" }, { "Europe/Chisinau", "E. Europe Standard Time" },
		{ "Africa/Cairo", "Egypt Standard Time" }, { "Asia/Jerusalem", "Israel Standard Time" },
		{ "Asia/Beirut", "Middle East Standard Time" }, { "Africa/Johannesburg", "South Africa Standard Time" },
		{ "Europe/Istanbul", "Turkey Standard Time" }, { "Europe/Moscow", "Russian Standard Time" },
		{ "Europe/Minsk", "Belarus Standard Time" }, { "Asia/Riyadh", "Arab Standard Time" },
		{ "Asia/Baghdad", "Arabic Standard Time" }, { "Africa/Nairobi", "E. Africa Standard Time" },
		{ "Asia/Tehran", "Iran Standard Time" }, { "Asia/Dubai", "Arabian Standard Time" },
		{ "Asia/Baku", "Azerbaijan Standard Time" }, { "Asia/Tbilisi", "Georgian Standard Time" },
		{ "Asia/Yerevan", "Caucasus Standard Time" }, { "Asia/Kabul", "Afghanistan Standard Time" },
		{ "Asia/Karachi", "Pakistan Standard Time" }, { "Asia/Tashkent", "West Asia Standard Time" },
		{ "Asia/Kolkata", "India Standard Time" }, { "Asia/Calcutta", "India Standard Time" },
		{ "Asia/Colombo", "Sri Lanka Standard Time" }, { "Asia/Kathmandu", "Nepal Standard Time" },
		{ "Asia/Almaty", "Central Asia Standard Time" }, { "Asia/Dhaka", "Bangladesh Standard Time" },
		{ "Asia/Yangon", "Myanmar Standard Time" }, { "Asia/Bangkok", "SE Asia Standard Time" },
		{ "Asia/Jakarta", "SE Asia Standard Time" }, { "Asia/Ho_Chi_Minh", "SE Asia Standard Time" },
		{ "Asia/Shanghai", "China Standard Time" }, { "Asia/Hong_Kong", "China Standard Time" },
		{ "Asia/Singapore", "Singapore Standard Time" }, { "Asia/Kuala_Lumpur", "Singapore Standard Time" },
		{ "Asia/Manila", "Singapore Standard Time" }, { "Australia/Perth", "W. Australia Standard Time" },
		{ "Asia/Taipei", "Taipei Standard Time" }, { "Asia/Ulaanbaatar", "Ulaanbaatar Standard Time" },
		{ "Asia/Tokyo", "Tokyo Standard Time" }, { "Asia/Seoul", "Korea Standard Time" },
		{ "Australia/Adelaide", "Cen. Australia Standard Time" }, { "Australia/Darwin", "AUS Central Standard Time" },
		{ "Australia/Brisbane", "E. Australia Standard Time" }, { "Australia/Sydney", "AUS Eastern Standard Time" },
		{ "Australia/Melbourne", "AUS Eastern Standard Time" }, { "Australia/Hobart", "Tasmania Standard Time" },
		{ "Pacific/Guam", "West Pacific Standard Time" }, { "Asia/Vladivostok", "Vladivostok Standard Time" },
		{ "Pacific/Auckland", "New Zealand Standard Time" }, { "Pacific/Fiji", "Fiji Standard Time" },
		{ "Pacific/Tongatapu", "Tonga Standard Time" }, { "Pacific/Honolulu", "Hawaiian Standard Time" },
		{ "America/Anchorage", "Alaskan Standard Time" }, { "America/Los_Angeles", "Pacific Standard Time" },
		{ "America/Vancouver", "Pacific Standard Time" }, { "America/Tijuana", "Pacific Standard Time (Mexico)" },
		{ "America/Phoenix", "US Mountain Standard Time" }, { "America/Denver", "Mountain Standard Time" },
		{ "America/Edmonton", "Mountain Standard Time" }, { "America/Chicago", "Central Standard Time" },
		{ "America/Winnipeg", "Central Standard Time" }, { "America/Mexico_City", "Central Standard Time (Mexico)" },
		{ "America/Regina", "Canada Central Standard Time" }, { "America/Guatemala", "Central America Standard Time" },
		{ "America/New_York", "Eastern Standard Time" }, { "America/Toronto", "Eastern Standard Time" },
		{ "America/Detroit", "Eastern Standard Time" }, { "America/Indiana/Indianapolis", "US Eastern Standard Time" },
		{ "America/Bogota", "SA Pacific Standard Time" }, { "America/Lima", "SA Pacific Standard Time" },
		{ "America/Havana", "Cuba Standard Time" }, { "America/Halifax", "Atlantic Standard Time" },
		{ "America/Caracas", "Venezuela Standard Time" }, { "America/La_Paz", "SA Western Standard Time" },
		{ "America/Santiago", "Pacific SA Standard Time" }, { "America/St_Johns", "Newfoundland Standard Time" },
		{ "America/Sao_Paulo", "E. South America Standard Time" }, { "America/Argentina/Buenos_Aires", "Argentina Standard Time" },
		{ "America/Buenos_Aires", "Argentina Standard Time" }, { "America/Montevideo", "Montevideo Standard Time" },
		{ "America/Cayenne", "SA Eastern Standard Time" }, { "America/Fortaleza", "SA Eastern Standard Time" },
		{ "America/Recife", "SA Eastern Standard Time" }, { "America/Bahia", "Bahia Standard Time" },
		{ "America/Manaus", "SA Western Standard Time" }, { "America/Godthab", "Greenland Standard Time" },
		{ "America/Nuuk", "Greenland Standard Time" }, { "Atlantic/Azores", "Azores Standard Time" },
		{ "Atlantic/Cape_Verde", "Cape Verde Standard Time" }, { "Africa/Casablanca", "Morocco Standard Time" },
	};
	char link[MAX_PATH] = "", *p;
	ssize_t len;
	size_t i;
	FILE* fd;

	/* /etc/timezone (Debian), else the /etc/localtime symlink */
	fd = fopen("/etc/timezone", "r");
	if (fd != NULL) {
		if (fgets(link, sizeof(link), fd) == NULL)
			link[0] = 0;
		fclose(fd);
		link[strcspn(link, "\r\n")] = 0;
	}
	if (link[0] == 0) {
		len = readlink("/etc/localtime", link, sizeof(link) - 1);
		if (len <= 0)
			return NULL;
		link[len] = 0;
	}
	p = strstr(link, "zoneinfo/");
	p = (p != NULL) ? &p[9] : link;
	for (i = 0; i < ARRAYSIZE(tz_map); i++)
		if (strcmp(p, tz_map[i].iana) == 0)
			return tz_map[i].windows;
	uprintf("WARNING: Could not map time zone '%s' to a Windows time zone", p);
	return NULL;
}

// Get the UI language provided by the ISO's boot.wim
char* GetUILanguage(void)
{
	static char ui_language[8];
	int r;
	char wim_path[4 * MAX_PATH] = "";
	char* xml = NULL;
	ezxml_t pxml = NULL;
	size_t xml_len;
	WIMStruct* wim = NULL;

	assert(safe_strlen(image_path) + 32 < ARRAYSIZE(wim_path));
	static_strcpy(ui_language, "en-US");
	static_strcpy(wim_path, image_path);
	static_strcat(wim_path, "|sources/boot.wim");

	r = wimlib_open_wimU(wim_path, 0, &wim);
	if (r != 0) {
		uprintf("Could not open WIM: Error %d", r);
		goto out;
	}

	r = wimlib_get_xml_data(wim, (void**)&xml, &xml_len);
	if (r != 0) {
		uprintf("Could not read WIM XML index: Error %d", r);
		goto out;
	}

	pxml = ezxml_parse_str((char*)xml, xml_len);
	if (pxml == NULL)
		goto out;

	char* lang = ezxml_get_val(pxml, "IMAGE", 1, "WINDOWS", 0, "LANGUAGES", 0, "LANGUAGE", -1);
	if (lang != NULL)
		static_strcpy(ui_language, lang);

out:
	ezxml_free(pxml);
	free(xml);
	wimlib_free(wim);
	return ui_language;
}

/// <summary>
/// Create an installation answer file containing the sections specified by the flags.
/// </summary>
/// <param name="arch">The processor architecture of the Windows image being used.</param>
/// <param name="flags">A bitmask representing the sections to enable.
/// See "Windows User Experience flags and masks" from rufus.h</param>
/// <returns>The path of a newly created answer file on success or NULL on error.</returns>
char* CreateUnattendXml(int arch, int flags)
{
	const static char* xml_arch_names[5] = { "x86", "amd64", "arm", "arm64" };
	const static char* unallowed_account_names[] = {
		// From https://learn.microsoft.com/en-us/archive/technet-wiki/13813.localized-names-for-administrator-account-in-windows
		"Administrator", "Järjestelmänvalvoja", "Administrateur", "Rendszergazda", "Administrador", "Администратор", "Administratör",
		"Guest", "DefaultAccount", "WDAGUtilityAccount", "HelpAssistant", "KRBTGT", "Local", "NONE", "SYSTEM"
	};
	static char path[MAX_PATH], tmp[MAX_PATH];
	const char* tzstr;
	FILE* fd;
	int i, order;
	unattend_xml_flags = flags;
	StrArray commands = { 0 };
	if (arch < ARCH_X86_32 || arch > ARCH_ARM_64 || flags == 0) {
		uprintf("Note: No Windows User Experience options selected");
		return NULL;
	}
	arch--;
	// coverity[swapped_arguments]
	if (GetTempFileNameU(temp_dir, APPLICATION_NAME, 0, path) == 0)
		return NULL;
	fd = fopen(path, "w");
	if (fd == NULL)
		return NULL;

	uprintf("Selected Windows User Experience options:");
	fprintf(fd, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
	fprintf(fd, "<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">\n");

	StrArrayCreate(&commands, 16);
	if (flags & UNATTEND_WINPE_SETUP_MASK) {
		fprintf(fd, "  <settings pass=\"windowsPE\">\n");
		fprintf(fd, "    <component name=\"Microsoft-Windows-Setup\" processorArchitecture=\"%s\" language=\"neutral\" "
			"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
			"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
		// WinPE will complain if we don't provide a product key. *Any* product key. This is soooo idiotic...
		fprintf(fd, "      <UserData>\n");
		fprintf(fd, "        <AcceptEula>true</AcceptEula>\n");
		fprintf(fd, "        <ProductKey>\n");
		fprintf(fd, "          <Key />\n");
		fprintf(fd, "        </ProductKey>\n");
		fprintf(fd, "      </UserData>\n");
		if (flags & UNATTEND_SILENT_INSTALL) {
			uprintf("• ⚠Silent Install⚠");
			// Automatically partition the disk and set the installation target
			fprintf(fd, "      <DiskConfiguration>\n");
			fprintf(fd, "        <WillShowUI>OnError</WillShowUI>\n");
			if (flags & UNATTEND_DISABLE_BITLOCKER)
				fprintf(fd, "        <DisableEncryptedDiskProvisioning>true</DisableEncryptedDiskProvisioning>\n");
			// The following ensures that we display the disk selection screen if only the boot media is
			// available (i.e. if the system does not see any target for installation) or (in most cases)
			// if the user happens to have more than one disk connected besides the instal USB. In that
			// case the label assignment below fails and the UI is displayed allowing the user to provide
			// drivers or pick their target disk explicitly.
			// This also prevents the install media from being scratched.
			// With a special mention to Claude AI, that explicitly said this could not be accomplished...
			// Note that this may result in the disk partition screen being produced on systems that have
			// card readers, but we'd rather inconvenience a few people, to prevent potential data loss,
			// than the opposite. Also note that you do *NOT* want to try to use drive letter mapping for
			// the detection here, as if your drive isn't of type FIXED, the file copy steps bails out at
			// 75%. See https://github.com/pbatard/rufus/issues/2960 for more details.
			fprintf(fd, "        <Disk wcm:action=\"modify\">\n");
			fprintf(fd, "          <DiskID>1</DiskID> \n");
			fprintf(fd, "          <ModifyPartitions>\n");
			fprintf(fd, "            <ModifyPartition wcm:action=\"modify\">\n");
			fprintf(fd, "              <Order>1</Order>\n");
			fprintf(fd, "              <PartitionID>2</PartitionID>\n");
			fprintf(fd, "              <Label>RUFUS_BOOT</Label>\n");
			fprintf(fd, "            </ModifyPartition>\n");
			fprintf(fd, "          </ModifyPartitions>\n");
			fprintf(fd, "        </Disk>\n");
			fprintf(fd, "        <Disk wcm:action=\"add\">\n");
			fprintf(fd, "          <DiskID>0</DiskID> \n");
			fprintf(fd, "          <WillWipeDisk>true</WillWipeDisk> \n");
			fprintf(fd, "          <CreatePartitions>\n");
			// NB: Don't bother creating a recovery partition. Windows setup will do it for us.
			fprintf(fd, "            <CreatePartition wcm:action=\"add\">\n");
			fprintf(fd, "              <Order>1</Order>\n");
			fprintf(fd, "              <Type>EFI</Type>\n");
			fprintf(fd, "              <Size>260</Size>\n");
			fprintf(fd, "            </CreatePartition>\n");
			fprintf(fd, "            <CreatePartition wcm:action=\"add\">\n");
			fprintf(fd, "              <Order>2</Order>\n");
			fprintf(fd, "              <Type>MSR</Type>\n");
			fprintf(fd, "              <Size>16</Size>\n");
			fprintf(fd, "            </CreatePartition>\n");
			fprintf(fd, "            <CreatePartition wcm:action=\"add\">\n");
			fprintf(fd, "              <Order>3</Order>\n");
			fprintf(fd, "              <Type>Primary</Type>\n");
			fprintf(fd, "              <Extend>true</Extend>\n");
			fprintf(fd, "            </CreatePartition>\n");
			fprintf(fd, "          </CreatePartitions>\n");
			fprintf(fd, "          <ModifyPartitions>\n");
			fprintf(fd, "            <ModifyPartition wcm:action=\"add\">\n");
			fprintf(fd, "              <Order>1</Order>\n");
			fprintf(fd, "              <PartitionID>1</PartitionID>\n");
			fprintf(fd, "              <Label>EFI</Label>\n");
			fprintf(fd, "              <Format>FAT32</Format>\n");
			fprintf(fd, "            </ModifyPartition>\n");
			fprintf(fd, "            <ModifyPartition wcm:action=\"add\">\n");
			fprintf(fd, "              <Order>2</Order>\n");
			fprintf(fd, "              <PartitionID>3</PartitionID>\n");
			fprintf(fd, "              <Label>Windows</Label>\n");
			fprintf(fd, "              <Letter>C</Letter>\n");
			fprintf(fd, "              <Format>NTFS</Format>\n");
			fprintf(fd, "            </ModifyPartition>\n");
			fprintf(fd, "          </ModifyPartitions>\n");
			fprintf(fd, "        </Disk>\n");
			fprintf(fd, "      </DiskConfiguration>\n");
			fprintf(fd, "      <ImageInstall>\n");
			fprintf(fd, "        <OSImage>\n");
			fprintf(fd, "          <WillShowUI>OnError</WillShowUI>\n");
			fprintf(fd, "          <InstallFrom>\n");
			fprintf(fd, "            <MetaData wcm:action=\"add\">\n");
			fprintf(fd, "              <Key>/IMAGE/INDEX</Key>\n");
			fprintf(fd, "              <Value>%d</Value>\n", unattend_edition_index);
			fprintf(fd, "            </MetaData>\n");
			fprintf(fd, "          </InstallFrom>\n");
			fprintf(fd, "          <InstallTo>\n");
			fprintf(fd, "            <DiskID>0</DiskID>\n");
			fprintf(fd, "            <PartitionID>3</PartitionID>\n");
			fprintf(fd, "          </InstallTo>\n");
			fprintf(fd, "        </OSImage>\n");
			fprintf(fd, "      </ImageInstall>\n");
		}
		if (flags & UNATTEND_SECUREBOOT_TPM_MINRAM) {
			// This part produces the unbecoming display of a command prompt window during initial setup as well
			// as alters the layout and options of the initial Windows installer screens, which may scare users.
			// So, in format.c, we'll try to insert the registry keys directly and drop this section. However,
			// because Microsoft prevents Store apps from editing an offline registry, we do need this fallback.
			// NB: We could probably avoid this by running powershell with -WindowStyle "Hidden" but hey...
			uprintf("• Bypass SB/TPM/RAM");
			order = 1;
			removable_section[0] = (uint32_t)ftell(fd);
			fprintf(fd, "      <RunSynchronous>\n");
			for (i = 0; i < ARRAYSIZE(bypass_name); i++) {
				fprintf(fd, "        <RunSynchronousCommand wcm:action=\"add\">\n");
				fprintf(fd, "          <Order>%d</Order>\n", order++);
				fprintf(fd, "          <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v %s /t REG_DWORD /d 1 /f</Path>\n", bypass_name[i]);
				fprintf(fd, "        </RunSynchronousCommand>\n");
			}
			fprintf(fd, "      </RunSynchronous>\n");
			removable_section[1] = (uint32_t)ftell(fd);
		}
		fprintf(fd, "    </component>\n");
		if (flags & UNATTEND_SILENT_INSTALL) {
			fprintf(fd, "    <component name=\"Microsoft-Windows-International-Core-WinPE\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			// Set the language from the <LANGUAGE> tag of the boot.wim index
			fprintf(fd, "      <UILanguage>%s</UILanguage>\n", GetUILanguage());
			fprintf(fd, "    </component>\n");
		}
		fprintf(fd, "  </settings>\n");
	}

	if (flags & UNATTEND_SPECIALIZE_DEPLOYMENT_MASK) {
		fprintf(fd, "  <settings pass=\"specialize\">\n");
		fprintf(fd, "    <component name=\"Microsoft-Windows-Deployment\" processorArchitecture=\"%s\" language=\"neutral\" "
			"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
			"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
		// This part was picked from https://github.com/AveYo/MediaCreationTool.bat/blob/main/bypass11/AutoUnattend.xml
		// NB: This is INCOMPATIBLE with S-Mode below
		if (flags & UNATTEND_NO_ONLINE_ACCOUNT) {
			StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\OOBE\" /v BypassNRO /t REG_DWORD /d 1 /f", TRUE);
			uprintf("• Bypass online account requirement");
		}
		if (flags & UNATTEND_QOL_ENHANCEMENTS) {
			uprintf("• QoL: Disable OneDrive and Outlook by default");
			// Remove OneDrive
			StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\OneDrive\" /v DisableFileSyncNGSC /t REG_DWORD /d 1 /f", TRUE);
			StrArrayAdd(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Remove-Item -Path $env:SystemRoot\\System32\\OneDriveSetup.exe -Force -Confirm:$false; "
				"Remove-Item -Path $env:SystemRoot\\SysWOW64\\OneDriveSetup.exe -Force -Confirm:$false;\"", TRUE);
			// Remove Outlook. How the frig is forcing Outlook on users legal when MS got pinned for bundling IE with Windows?
			StrArrayAdd(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Get-AppxProvisionedPackage -Online | Where-Object {$_.PackageName -like '*Outlook*'} |"
				" Remove-AppxProvisionedPackage -Online\"", TRUE);
			StrArrayAdd(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Get-AppxPackage -AllUsers *Outlook* | Remove-AppxPackage -AllUsers\"", TRUE);
			// Same for Teams. Also: "HEY, MICROSOFT, SCREW YOU!!!!"
			StrArrayAdd(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Get-AppxProvisionedPackage -Online | Where-Object {$_.PackageName -like '*Teams*'} |"
				" Remove-AppxProvisionedPackage -Online\"", TRUE);
			StrArrayAdd(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Get-AppxPackage -AllUsers *Teams* | Remove-AppxPackage -AllUsers\"", TRUE);
		}
		// Now that we have all the commands to run, create the RunSynchronous section.
		for (order = 1; order <= (int)commands.Index; order++) {
			if (order == 1)
				fprintf(fd, "      <RunSynchronous>\n");
			fprintf(fd, "        <RunSynchronousCommand wcm:action=\"add\">\n");
			fprintf(fd, "          <Order>%d</Order>\n", order);
			fprintf(fd, "          <Path>%s</Path>\n", commands.String[order - 1]);
			fprintf(fd, "        </RunSynchronousCommand>\n");
			if (order == commands.Index)
				fprintf(fd, "      </RunSynchronous>\n");
		}
		fprintf(fd, "    </component>\n");
		fprintf(fd, "  </settings>\n");
		StrArrayClear(&commands);
	}

	if (flags & UNATTEND_OOBE_MASK) {
		fprintf(fd, "  <settings pass=\"oobeSystem\">\n");
		if (flags & UNATTEND_OOBE_SHELL_SETUP_MASK) {
			fprintf(fd, "    <component name=\"Microsoft-Windows-Shell-Setup\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			// We should always have UNATTEND_NO_DATA_COLLECTION if UNATTEND_SILENT_INSTALL is set but whatever...
			if (flags & (UNATTEND_NO_DATA_COLLECTION | UNATTEND_SILENT_INSTALL)) {
				uprintf("• Disable data collection");
				fprintf(fd, "      <OOBE>\n");
				fprintf(fd, "        <HideEULAPage>true</HideEULAPage>\n");
				// https://docs.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-shell-setup-oobe-protectyourpc
				// It is really super disingenuous of Microsoft to call this option "ProtectYourPC", when it's really only about
				// data collection. But of course, if it was called "AllowDataCollection", everyone would turn it off...
				fprintf(fd, "        <ProtectYourPC>3</ProtectYourPC>\n");
				// Without these, the "Let's connect you to a network" can appear in silent installs when Ethernet is disconnected.
				if (flags & UNATTEND_SILENT_INSTALL) {
					fprintf(fd, "        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>\n");
					fprintf(fd, "        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>\n");
				}
				fprintf(fd, "      </OOBE>\n");
			}
			if (flags & UNATTEND_DUPLICATE_LOCALE) {
				// PORT: map the host's IANA time zone to its Windows name
				if ((tzstr = GetWindowsTimeZone()) == NULL) {
					uprintf("WARNING: Could not retrieve current timezone");
				} else {
					fprintf(fd, "      <TimeZone>%s</TimeZone>\n", tzstr);
				}
			}
			if (flags & UNATTEND_SET_USER) {
				for (i = 0; (i < ARRAYSIZE(unallowed_account_names)) && (strcasecmp(unattend_username, unallowed_account_names[i]) != 0); i++);
				if (i < ARRAYSIZE(unallowed_account_names)) {
					uprintf("WARNING: '%s' is not allowed as local account name - Option ignored", unattend_username);
				} else if (unattend_username[0] != 0) {
					char* org_username = safe_strdup(unattend_username);
					filter_chars(unattend_username, USERNAME_INVALID_CHARS, '_');
					uprintf("• Use '%s' for local account name", unattend_username);
					if (strcmp(org_username, unattend_username) != 0)
						uprintf("WARNING: Local account name contained unallowed characters and has been sanitized");
					free(org_username);
					// If we create a local account in unattend.xml, then we can get Windows 11
					// 22H2 to skip MSA even if the network is connected during installation.
					fprintf(fd, "      <UserAccounts>\n");
					fprintf(fd, "        <LocalAccounts>\n");
					fprintf(fd, "          <LocalAccount wcm:action=\"add\">\n");
					fprintf(fd, "            <Name>%s</Name>\n", unattend_username);
					fprintf(fd, "            <DisplayName>%s</DisplayName>\n", unattend_username);
					fprintf(fd, "            <Group>Administrators;Power Users</Group>\n");
					// Sets an empty password for the account (which, in Microsoft's convoluted ways,
					// needs to be initialized to the Base64 encoded UTF-16 string "Password").
					// The use of an empty password has both the advantage of not having to ask users
					// to type in a password in Rufus (which they might be weary of) as well as allowing
					// automated logon during setup.
					fprintf(fd, "            <Password>\n");
					fprintf(fd, "              <Value>UABhAHMAcwB3AG8AcgBkAA==</Value>\n");
					fprintf(fd, "              <PlainText>false</PlainText>\n");
					fprintf(fd, "            </Password>\n");
					fprintf(fd, "          </LocalAccount>\n");
					fprintf(fd, "        </LocalAccounts>\n");
					fprintf(fd, "      </UserAccounts>\n");
					// Since we set a blank password, we'll ask the user to change it at next logon.
					// NB: In case you wanna try, please be aware that Microsoft doesn't let you have multiple
					// <FirstLogonCommands> sections in unattend.xml. Don't ask me how I know... :(
					static_sprintf(tmp, "net user \"%s\" /logonpasswordchg:yes", unattend_username);
					StrArrayAdd(&commands, tmp, TRUE);
					// The `net user` command above might reset the password expiration to 90 days...
					// To alleviate that, blanket set passwords on the target machine to never expire.
					StrArrayAdd(&commands, "net accounts /maxpwage:unlimited", TRUE);
				}
			}
			// Apply SkuSiPolicy.p7b, if the user requested it and we're not creating a Windows To Go drive.
			// See https://support.microsoft.com/kb/5042562. We do it post install, on first logon, because
			// we'd have to patch tons of files otherwise. And we *ALWAYS* use the installed system's
			// SkuSiPolicy.p7b instead of the host system's, even if the latter might be more recent, on
			// account that the bootloaders from the installed system might be trailing behind, and wills
			// produce the 0xc0000428 signature validation error on (re)boot if the system hasn't gone
			// through a full Windows Update cycle.
			if (flags & UNATTEND_APPLY_SKUSIPOLICY) {
				uprintf("• Apply SkuSiPolicy.p7b");
				StrArrayAdd(&commands, "cmd /c mountvol S: /S &amp;&amp; "
					"copy %WINDIR%\\system32\\SecureBootUpdates\\SkuSiPolicy.p7b S:\\EFI\\Microsoft\\Boot &amp;&amp; "
					"mountvol S: /D", TRUE);
			}
			if (flags & UNATTEND_QOL_ENHANCEMENTS) {
				uprintf("• QoL: Disable Fast Startup, Copilot, Recommendations, News and Teams by default");
				// Disable Fast Startup 
				StrArrayAdd(&commands, "reg add \"HKLM\\System\\CurrentControlSet\\Control\\Session Manager\\Power\" "
					"/v HiberbootEnabled /t REG_DWORD /d 0 /f", TRUE);
				// Disable Copilot and set search as an icon rather than the default real-estate gobbler
				StrArrayAdd(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" "
					"/v ShowCopilotButton /t REG_DWORD /d 0 /f", TRUE);
				StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\WindowsCopilot\" "
					"/v TurnOffWindowsCopilot /t REG_DWORD /d 1 /f", TRUE);
				StrArrayAdd(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Search\" "
					"/v SearchboxTaskbarMode /t REG_DWORD /d 1 /f", TRUE);
				StrArrayAdd(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Search\" "
					"/v SearchboxTaskbarModeCache /t REG_DWORD /d 1 /f", TRUE);
				// Disable Windows Phone and similarly pushed unwanted crap
				StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\CloudContent\" "
					"/v DisableWindowsConsumerFeatures /t REG_DWORD /d 1 /f", TRUE);
				// Disable ads in menu
				StrArrayAdd(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager\" "
					"/v SystemPaneSuggestionsEnabled /t REG_DWORD /d 0 /f", TRUE);
				StrArrayAdd(&commands, "reg add \"HKCU\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Search\" "
					"/v BingSearchEnabled /t REG_DWORD /d 0 /f", TRUE);
				// Disable auto installation of manufacturer's crappy apps when plugging new hardware
				StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\Device Metadata\" "
					"/v PreventDeviceMetadataFromNetwork /t REG_DWORD /d 1 /f", TRUE);
				// Disable News
				StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Dsh\" "
					"/v AllowNewsAndInterests /t REG_DWORD /d 0 /f", TRUE);
				StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\Windows Feeds\" "
					"/v EnableFeeds /t REG_DWORD /d 0 /f", TRUE);
				// Disable Teams
				StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Communications\" "
					"/v ConfigureChatAutoInstall /t REG_DWORD /d 0 /f", TRUE);
				// Prevent frigging Outlook from being forced into the user's taskbar
				StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\CloudContent\" "
					"/v DisableCloudOptimizedContent /t REG_DWORD /d 1 /f", TRUE);
				// Skip Edge's first run dialog
				StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Edge\" "
					"/v HideFirstRunExperience /t REG_DWORD /d 1 /f", TRUE);
				uprintf("• QoL: More pins for the Start Menu and enable useful shortcuts");
				// More pins by default on the Start Menu
				// https://learn.microsoft.com/en-us/windows/apps/develop/settings/settings-windows-11
				StrArrayAdd(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" "
					"/v Start_Layout /t REG_DWORD /d 1 /f", TRUE);
				// Add Documents, Downloads, Network, Personal Folder, File Explorer and Settings as start menu shortcuts
				// I wish there was a more transparent way of doing it, but Microsoft made it obscure. Oh, and for some
				// reason, feeding the full hex string through 'reg add' doesn't create the key when ran through unattend
				// (but doesn't produce an error and works post logon). PowerShell it is then...
				StrArrayAdd(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
					"\"Set-ItemProperty -Path 'Registry::HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Start' "
					"-Name 'VisiblePlaces' -Value $([convert]::FromBase64String('ztU0LVr6Q0WC8iLm6vd3PC+zZ+PeiVVDv85h83sYqTe8JIo"
					"UDNaJQqCAbtm7okiCRIF1/g0IrkKL2jTtl7ZjlEqwvXRK+WhPi9ZDmAcdqLyGCHNSqlFDQp97J3ZYRlnU')) -Type 'Binary'\"", TRUE);
				// Restore the classic right click context menu
				uprintf("• QoL: Restore classic context menu");
				StrArrayAdd(&commands, "reg add \"HKCU\\Software\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\\InprocServer32\" "
					"/ve /t REG_SZ /d \"\" /f", TRUE);
			}
			// Now that we have all the commands to run, create the FirstLogonCommands section.
			for (order = 1; order <= (int)commands.Index; order++) {
				if (order == 1)
					fprintf(fd, "      <FirstLogonCommands>\n");
				fprintf(fd, "        <SynchronousCommand wcm:action=\"add\">\n");
				fprintf(fd, "          <Order>%d</Order>\n", order);
				fprintf(fd, "          <CommandLine>%s</CommandLine>\n", commands.String[order - 1]);
				fprintf(fd, "        </SynchronousCommand>\n");
				if (order == commands.Index)
					fprintf(fd, "      </FirstLogonCommands>\n");
			}
			fprintf(fd, "    </component>\n");
			StrArrayClear(&commands);
		}
		if (flags & UNATTEND_OOBE_INTERNATIONAL_MASK) {
			uprintf("• Use the same regional options as this user's");
			fprintf(fd, "    <component name=\"Microsoft-Windows-International-Core\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			// What a frigging mess retreiving and trying to match the various locales
			// Microsoft has made. And, *NO*, the new User Language Settings have not
			// improved things in the slightest. They made it much worse for developers!
			// PORT: Linux equivalents of the registry/NLS values (keyboard layout
			// ID from XKB, locale names from the Linux locale)
			fprintf(fd, "      <InputLocale>%08x</InputLocale>\n", GetKeyboardLayoutId());
			fprintf(fd, "      <SystemLocale>%s</SystemLocale>\n", GetLocaleName());
			fprintf(fd, "      <UserLocale>%s</UserLocale>\n", GetLocaleName());
			fprintf(fd, "      <UILanguage>%s</UILanguage>\n", GetLocaleName());
			fprintf(fd, "      <UILanguageFallback>%s</UILanguageFallback>\n", "en-US");
			fprintf(fd, "    </component>\n");
		}
		if (flags & UNATTEND_DISABLE_BITLOCKER) {
			uprintf("• Disable BitLocker");
			fprintf(fd, "    <component name=\"Microsoft-Windows-SecureStartup-FilterDriver\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			fprintf(fd, "      <PreventDeviceEncryption>true</PreventDeviceEncryption>\n");
			fprintf(fd, "    </component>\n");
			fprintf(fd, "    <component name=\"Microsoft-Windows-EnhancedStorage-Adm\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			fprintf(fd, "      <TCGSecurityActivationDisabled>1</TCGSecurityActivationDisabled>\n");
			fprintf(fd, "    </component>\n");
		}
		fprintf(fd, "  </settings>\n");
	}

	if (flags & UNATTEND_OFFLINE_SERVICING_MASK) {
		fprintf(fd, "  <settings pass=\"offlineServicing\">\n");
		if (flags & UNATTEND_OFFLINE_INTERNAL_DRIVES) {
			uprintf("• Set internal drives offline");
			fprintf(fd, "    <component name=\"Microsoft-Windows-PartitionManager\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			fprintf(fd, "      <SanPolicy>4</SanPolicy>\n");
			fprintf(fd, "    </component>\n");
		}
		if (flags & UNATTEND_FORCE_S_MODE) {
			uprintf("• Enforce S Mode");
			fprintf(fd, "    <component name=\"Microsoft-Windows-CodeIntegrity\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			fprintf(fd, "      <SkuPolicyRequired>1</SkuPolicyRequired>\n");
			fprintf(fd, "    </component>\n");
		}
		fprintf(fd, "  </settings>\n");
	}

	if (flags & UNATTEND_USE_MS2023_BOOTLOADERS)
		uprintf("• Use 'Windows CA 2023' signed bootloaders");

	StrArrayDestroy(&commands);
	fprintf(fd, "</unattend>\n");
	fclose(fd);
	return path;
}

/// <summary>
/// Setup and patch WinPE for Windows XP bootable USBs.
/// </summary>
/// <param name="drive_letter">The letter identifying the target drive.</param>
/// <returns>TRUE on success, FALSE on error.</returns>
BOOL SetupWinPE(const char* drive_name)
{
	char src[MAX_PATH], dst[MAX_PATH];
	const char* basedir[3] = { "i386", "amd64", "minint" };
	const char* patch_str_org[2] = { "\\minint\\txtsetup.sif", "\\minint\\system32\\" };
	const char* patch_str_rep[2][2] = { { "\\i386\\txtsetup.sif", "\\i386\\system32\\" } ,
										{ "\\amd64\\txtsetup.sif", "\\amd64\\system32\\" } };
	const char* setupsrcdev = "SetupSourceDevice = \"\\device\\harddisk1\\partition1\"";
	const char* win_nt_bt_org = "$win_nt$.~bt";
	const char* rdisk_zero = "rdisk(0)";
	const LARGE_INTEGER liZero = { {0, 0} };
	HANDLE handle = INVALID_HANDLE_VALUE;
	DWORD i, j, size, read_size, index = 0;
	BOOL r = FALSE;
	char* buffer = NULL;

	if ((img_report.winpe & WINPE_AMD64) == WINPE_AMD64)
		index = 1;
	else if ((img_report.winpe & WINPE_MININT) == WINPE_MININT)
		index = 2;
	// Copy of ntdetect.com in root
	static_sprintf(src, "%s/%s/ntdetect.com", drive_name, basedir[2 * (index / 2)]);
	static_sprintf(dst, "%s/ntdetect.com", drive_name);
	CopyFileA(src, dst, TRUE);
	if (!img_report.uses_minint) {
		// Create a copy of txtsetup.sif, as we want to keep the i386/amd64 files unmodified
		static_sprintf(src, "%s/%s/txtsetup.sif", drive_name, basedir[index]);
		static_sprintf(dst, "%s/txtsetup.sif", drive_name);
		if (!CopyFileA(src, dst, TRUE)) {
			uprintf("Did not copy %s as %s: %s", src, dst, WindowsErrorString());
		}
		if (insert_section_data(dst, "[SetupData]", setupsrcdev, FALSE) == NULL) {
			uprintf("Failed to add SetupSourceDevice in %s", dst);
			goto out;
		}
		uprintf("Successfully added '%s' to %s", setupsrcdev, dst);
	}

	static_sprintf(src, "%s/%s/setupldr.bin", drive_name, basedir[2 * (index / 2)]);
	static_sprintf(dst, "%s/BOOTMGR", drive_name);
	if (!CopyFileA(src, dst, TRUE)) {
		uprintf("Did not copy %s as %s: %s", src, dst, WindowsErrorString());
	}

	// \minint with /minint option doesn't require further processing => return true
	// \minint and no \i386 without /minint is unclear => return error
	if (img_report.winpe & WINPE_MININT) {
		if (img_report.uses_minint) {
			uprintf("Detected \\minint directory with /minint option: nothing to patch");
			r = TRUE;
		} else if (!(img_report.winpe & (WINPE_I386 | WINPE_AMD64))) {
			uprintf("Detected \\minint directory only but no /minint option: not sure what to do");
		}
		goto out;
	}

	// At this stage we only handle \i386
	handle = CreateFileA(dst, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		uprintf("Could not open %s for patching: %s", dst, WindowsErrorString());
		goto out;
	}
	size = GetFileSize(handle, NULL);
	if (size == INVALID_FILE_SIZE) {
		uprintf("Could not get size for file %s: %s", dst, WindowsErrorString());
		goto out;
	}
	buffer = (char*)malloc(size);
	if (buffer == NULL)
		goto out;
	if ((!ReadFile(handle, buffer, size, &read_size, NULL)) || (size != read_size)) {
		uprintf("Could not read file %s: %s", dst, WindowsErrorString());
		goto out;
	}
	if (!SetFilePointerEx(handle, liZero, NULL, FILE_BEGIN)) {
		uprintf("Could not rewind file %s: %s", dst, WindowsErrorString());
		goto out;
	}

	// Patch setupldr.bin
	uprintf("Patching file %s", dst);
	// Remove CRC check for 32 bit part of setupldr.bin from Win2k3
	if ((size > 0x2061) && (buffer[0x2060] == 0x74) && (buffer[0x2061] == 0x03)) {
		buffer[0x2060] = 0xeb;
		buffer[0x2061] = 0x1a;
		uprintf("  0x00002060: 0x74 0x03 -> 0xEB 0x1A (disable Win2k3 CRC check)");
	}
	for (i = 1; i < size - 32; i++) {
		for (j = 0; j < ARRAYSIZE(patch_str_org); j++) {
			if (safe_strnicmp(&buffer[i], patch_str_org[j], strlen(patch_str_org[j]) - 1) == 0) {
				assert(index < 2);
				uprintf("  0x%08X: '%s' -> '%s'", i, &buffer[i], patch_str_rep[index][j]);
				strcpy(&buffer[i], patch_str_rep[index][j]);
				i += (DWORD)max(strlen(patch_str_org[j]), strlen(patch_str_rep[index][j]));	// in case org is a substring of rep
			}
		}
	}

	if (!img_report.uses_minint) {
		// Additional setupldr.bin/bootmgr patching
		for (i = 0; i < size - 32; i++) {
			// rdisk(0) -> rdisk(#) disk masquerading
			// NB: only the first one seems to be needed
			if (safe_strnicmp(&buffer[i], rdisk_zero, strlen(rdisk_zero) - 1) == 0) {
				buffer[i + 6] = 0x31;
				uprintf("  0x%08X: '%s' -> 'rdisk(%c)'", i, rdisk_zero, buffer[i + 6]);
			}
			// $WIN_NT$_~BT -> i386/amd64
			if (safe_strnicmp(&buffer[i], win_nt_bt_org, strlen(win_nt_bt_org) - 1) == 0) {
				uprintf("  0x%08X: '%s' -> '%s%s'", i, &buffer[i], basedir[index], &buffer[i + strlen(win_nt_bt_org)]);
				strcpy(&buffer[i], basedir[index]);
				// This ensures that we keep the terminator backslash
				buffer[i + strlen(basedir[index])] = buffer[i + strlen(win_nt_bt_org)];
				buffer[i + strlen(basedir[index]) + 1] = 0;
			}
		}
	}

	if (!WriteFileWithRetry(handle, buffer, size, NULL, WRITE_RETRIES)) {
		uprintf("Could not write patched file: %s", WindowsErrorString());
		goto out;
	}
	r = TRUE;

out:
	safe_closehandle(handle);
	safe_free(buffer);
	return r;
}

/// <summary>
/// Populate the img_report Window version from an install[.wim|.esd] XML index
/// </summary>
/// <param name="xml">The index XML data.</param>
/// <param name="xml_len">The length of the index XML data.</param>
/// <param name="index">The index of the occurrence to look for.</param>
static void PopulateWindowsVersionFromXml(const char* xml, size_t xml_len, int index)
{
	char* val;
	ezxml_t pxml = ezxml_parse_str((char*)xml, xml_len);
	if (pxml == NULL)
		return;

	val = ezxml_get_val(pxml, "IMAGE", index, "WINDOWS", 0, "VERSION", 0, "MAJOR", -1);
	img_report.win_version.major = (uint16_t)safe_atoi(val);
	val = ezxml_get_val(pxml, "IMAGE", index, "WINDOWS", 0, "VERSION", 0, "MINOR", -1);
	img_report.win_version.minor = (uint16_t)safe_atoi(val);
	val = ezxml_get_val(pxml, "IMAGE", index, "WINDOWS", 0, "VERSION", 0, "BUILD", -1);
	img_report.win_version.build = (uint16_t)safe_atoi(val);
	val = ezxml_get_val(pxml, "IMAGE", index, "WINDOWS", 0, "VERSION", 0, "SPBUILD", -1);
	img_report.win_version.revision = (uint16_t)safe_atoi(val);
	// Adjust versions so that we produce a more accurate report in the log
	// (and yeah, I know we won't properly report Server, but I don't care)
	if (img_report.win_version.major <= 5) {
		// Don't want to support XP or earlier
		img_report.win_version.major = 0;
		img_report.win_version.minor = 0;
	} else if (img_report.win_version.major == 6) {
		// Don't want to support Vista
		if (img_report.win_version.minor == 0) {
			img_report.win_version.major = 0;
		} else if (img_report.win_version.minor == 1) {
			img_report.win_version.major = 7;
			img_report.win_version.minor = 0;
		} else if (img_report.win_version.minor == 2) {
			img_report.win_version.major = 8;
			img_report.win_version.minor = 0;
		} else if (img_report.win_version.minor == 3) {
			img_report.win_version.major = 8;
			img_report.win_version.minor = 1;
		} else if (img_report.win_version.minor == 4) {
			img_report.win_version.major = 10;
			img_report.win_version.minor = 0;
		}
	} else if (img_report.win_version.major == 10) {
		if (img_report.win_version.build > 20000)
			img_report.win_version.major = 11;
	}
	ezxml_free(pxml);
}

/// <summary>
/// Populate the img_report Window version from an install[.wim|.esd].
/// </summary>
/// <param name="">(none)</param>
/// <returns>TRUE on success, FALSE if we couldn't populate the version.</returns>
BOOL PopulateWindowsVersion(void)
{
	int r;
	char wim_path[4 * MAX_PATH] = "";
	char* xml = NULL;
	size_t xml_len;
	WIMStruct* wim = NULL;

	memset(&img_report.win_version, 0, sizeof(img_report.win_version));

	assert(safe_strlen(image_path) + 1 < ARRAYSIZE(wim_path));
	static_strcpy(wim_path, image_path);
	if (!img_report.is_windows_img) {
		// PORT: wininst_path[] is already relative to the ISO root (upstream's [3])
		assert(safe_strlen(image_path) + safe_strlen(img_report.wininst_path[0]) + 1 < ARRAYSIZE(wim_path));
		static_strcat(wim_path, "|");
		static_strcat(wim_path, img_report.wininst_path[0]);
	}

	r = wimlib_open_wimU(wim_path, 0, &wim);
	if (r != 0) {
		uprintf("Could not open WIM: Error %d", r);
		goto out;
	}

	r = wimlib_get_xml_data(wim, (void**)&xml, &xml_len);
	if (r != 0) {
		uprintf("Could not read WIM XML index: Error %d", r);
		goto out;
	}

	PopulateWindowsVersionFromXml(xml, xml_len, 0);

out:
	free(xml);
	wimlib_free(wim);

	return ((img_report.win_version.major != 0) && (img_report.win_version.build != 0));
}

int GetEditions(StrArray* version_name, StrArray* version_index)
{
	int i, r, n = -1;
	WIMStruct* wim = NULL;
	selection_dialog_options_t selection = { 0 };
	const char* edition_suffix;
	char *edition_name;
	char wim_path[4 * MAX_PATH] = "", *xml = NULL;
	size_t xml_len;
	BOOL bNonStandard = FALSE;
	ezxml_t index = NULL, image = NULL;

	// If we have multiple windows install images, ask the user the one to use
	if (img_report.wininst_index > 1) {
		StrArrayCreate(&selection.choices, 16);
		for (i = 0; i < img_report.wininst_index; i++)
			StrArrayAdd(&selection.choices, img_report.wininst_path[i], TRUE);
		wininst_index = _log2(SelectionDialog(lmprintf(MSG_130), lmprintf(MSG_131), &selection));
		StrArrayDestroy(&selection.choices);
		if (wininst_index < 0)
			return -2;
		if (wininst_index >= MAX_WININST)
			wininst_index = 0;
	}

	static_strcpy(wim_path, image_path);
	if (!img_report.is_windows_img) {
		static_strcat(wim_path, "|");
		static_strcat(wim_path, img_report.wininst_path[wininst_index]);
	}

	r = wimlib_open_wim(wim_path, 0, &wim);
	if (r != 0) {
		uprintf("Could not open WIM: %d", r);
		goto out;
	}
	r = wimlib_get_xml_data(wim, (void**)&xml, &xml_len);
	if (r != 0) {
		uprintf("Could not read WIM XML index: %d", r);
		goto out;
	}

	index = ezxml_parse_str((char*)xml, xml_len);
	if (index == NULL) {
		uprintf("Could not parse WIM XML");
		goto out;
	}

	// PORT: no host Windows edition to match - use the most common one
	edition_suffix = "Pro";
	unattend_edition_index = 1;
	for (n = 0, image = ezxml_child(index, "IMAGE");
		StrArrayAdd(version_index, ezxml_attr(image, "INDEX"), TRUE) >= 0;
		image = image->next, n++) {
		// Try to match this host's edition with an index from the image
		edition_name = ezxml_child_val(image, "NAME");
		if (edition_name != NULL) {
			if (strlen(edition_name) > safe_strlen(edition_suffix) &&
				strcasecmp(&edition_name[strlen(edition_name) - safe_strlen(edition_suffix)], edition_suffix) == 0)
				unattend_edition_index = atoi(ezxml_attr(image, "INDEX")) - 1;
		}
		// Some people are apparently creating *unofficial* Windows ISOs that don't have DISPLAYNAME elements.
		// If we are parsing such an ISO, try to fall back to using DESCRIPTION.
		if (StrArrayAdd(version_name, ezxml_child_val(image, "DISPLAYNAME"), TRUE) < 0) {
			if (StrArrayAdd(version_name, ezxml_child_val(image, "DESCRIPTION"), TRUE) < 0) {
				uprintf("WARNING: Could not find a description for image index %d", n + 1);
				StrArrayAdd(version_name, "Unknown Windows Version", TRUE);
			}
			bNonStandard = TRUE;
		}
	}
	if (bNonStandard)
		uprintf("WARNING: Nonstandard Windows image (missing <DISPLAYNAME> entries)");

out:
	free(xml);
	ezxml_free(index);
	wimlib_free(wim);
	return n;
}

/// <summary>
/// Checks which versions of Windows are available in an install image
/// to set our extraction index. Asks the user to select one if needed.
/// </summary>
/// <param name="">(none)</param>
/// <returns>-2 on user cancel, -1 on other error, >=0 on success.</returns>
int SetWinToGoIndex(void)
{
	int i, r;
	const char* edition_suffix;
	char* edition_name;
	WIMStruct* wim = NULL;
	char wim_path[4 * MAX_PATH] = "", *xml = NULL;
	size_t xml_len;
	StrArray edition_index = { 0 };
	BOOL bNonStandard = FALSE;
	ezxml_t index = NULL, image = NULL;
	selection_dialog_options_t selection = { 0 };

	// Sanity checks
	wintogo_index = -1;
	wininst_index = 0;
	// PORT: creating the BCD store of a Windows To Go drive requires the
	// Windows bcdboot tool, which has no Linux equivalent
	uprintf("Windows To Go is not supported on Linux (it requires the Windows 'bcdboot' tool)");
	return -1;
	if (fs_type != FS_NTFS)
		return -1;

	// If we have multiple windows install images, ask the user the one to use
	if (img_report.wininst_index > 1) {
		StrArrayCreate(&selection.choices, 16);
		for (i = 0; i < img_report.wininst_index; i++)
			StrArrayAdd(&selection.choices, img_report.wininst_path[i], TRUE);
		wininst_index = _log2(SelectionDialog(lmprintf(MSG_130), lmprintf(MSG_131), &selection));
		StrArrayDestroy(&selection.choices);
		if (wininst_index < 0)
			return -2;
		if (wininst_index >= MAX_WININST)
			wininst_index = 0;
	}

	static_strcpy(wim_path, image_path);
	if (!img_report.is_windows_img) {
		static_strcat(wim_path, "|");
		static_strcat(wim_path, img_report.wininst_path[wininst_index]);
	}

	r = wimlib_open_wim(wim_path, 0, &wim);
	if (r != 0) {
		uprintf("Could not open WIM: %d", r);
		goto out;
	}
	r = wimlib_get_xml_data(wim, (void**)&xml, &xml_len);
	if (r != 0) {
		uprintf("Could not read WIM XML index: %d", r);
		goto out;
	}

	StrArrayCreate(&selection.choices, 16);
	StrArrayCreate(&edition_index, 16);
	index = ezxml_parse_str((char*)xml, xml_len);
	if (index == NULL) {
		uprintf("Could not parse WIM XML");
		goto out;
	}

	// PORT: no host Windows edition to match - use the most common one
	edition_suffix = "Pro";
	unattend_edition_index = 1;
	for (i = 0, image = ezxml_child(index, "IMAGE");
		StrArrayAdd(&edition_index, ezxml_attr(image, "INDEX"), TRUE) >= 0;
		image = image->next, i++) {
		// Try to match this host's edition with an index from the image
		edition_name = ezxml_child_val(image, "NAME");
		if (edition_name != NULL) {
			if (strlen(edition_name) > safe_strlen(edition_suffix) &&
				strcasecmp(&edition_name[strlen(edition_name) - safe_strlen(edition_suffix)], edition_suffix) == 0)
				unattend_edition_index = atoi(ezxml_attr(image, "INDEX")) - 1;
		}
		// Some people are apparently creating *unofficial* Windows ISOs that don't have DISPLAYNAME elements.
		// If we are parsing such an ISO, try to fall back to using DESCRIPTION.
		if (StrArrayAdd(&selection.choices, ezxml_child_val(image, "DISPLAYNAME"), TRUE) < 0) {
			if (StrArrayAdd(&selection.choices, ezxml_child_val(image, "DESCRIPTION"), TRUE) < 0) {
				uprintf("WARNING: Could not find a description for image index %d", i + 1);
				StrArrayAdd(&selection.choices, "Unknown Windows Version", TRUE);
			}
			bNonStandard = TRUE;
		}
	}
	if (bNonStandard)
		uprintf("WARNING: Nonstandard Windows image (missing <DISPLAYNAME> entries)");

	selection.mask = 1 << unattend_edition_index;
	if (i > 1)
		// NB: _log2 returns -2 if SelectionDialog() returns negative (user cancelled)
		i = _log2(SelectionDialog(lmprintf(MSG_291), lmprintf(MSG_292), &selection)) + 1;
	if (i < 0)
		wintogo_index = -2;	// Cancelled by the user
	else if (i == 0)
		wintogo_index = 1;
	else
		wintogo_index = atoi(edition_index.String[i - 1]);
	if (i > 0) {
		// re-populate the version data from the selected XML index
		PopulateWindowsVersionFromXml(xml, xml_len, i - 1);
		// If we couldn't obtain the major and build, we have a problem
		if (img_report.win_version.major == 0 || img_report.win_version.build == 0)
			uprintf("WARNING: Could not obtain version information from XML index (Nonstandard Windows image?)");
		uprintf("Will use '%s' (Build: %d, Index %s) for Windows To Go",
			selection.choices.String[i - 1], img_report.win_version.build, edition_index.String[i - 1]);
		// Need Windows 10 Creator Update or later for boot on REMOVABLE to work
		if ((img_report.win_version.build < 15000) && !IsFixedMedia(SelectedDrive.DeviceNumber)) {
			if (Notification(MB_YESNO | MB_ICONWARNING, lmprintf(MSG_190), lmprintf(MSG_098)) != IDYES)
				wintogo_index = -2;
		}
		// Display a notice about WppRecorder.sys for 1809 ISOs
		if (img_report.win_version.build == 17763) {
			notification_info more_info;
			more_info.id = MORE_INFO_URL;
			more_info.url = WPPRECORDER_MORE_INFO_URL;
			NotificationEx(MB_ICONINFORMATION | MB_CLOSE, NULL, &more_info, lmprintf(MSG_128, "Windows To Go"), lmprintf(MSG_133));
		}
	}

out:
	StrArrayDestroy(&selection.choices);
	StrArrayDestroy(&edition_index);
	free(xml);
	ezxml_free(index);
	wimlib_free(wim);
	return wintogo_index;
}

/// <summary>
/// Setup a Windows To Go drive according to the official Microsoft instructions detailed at:
/// https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-10/deployment/windows-to-go/deploy-windows-to-go
/// Note that as opposed to the technet guide above we use bcdedit rather than 'unattend.xml'
/// to disable the recovery environment.
/// </summary>
/// <param name="DriveIndex">The Rufus drive index for the target media.</param>
/// <param name="drive_name">The path of the target media.</param>
/// <param name="use_esp">Whether to create an ESP on the target media.</param>
/// <returns>TRUE on success, FALSE on error.</returns>
BOOL SetupWinToGo(DWORD DriveIndex, const char* drive_name, BOOL use_esp)
{
	// PORT: see SetWinToGoIndex()
	(void)DriveIndex; (void)drive_name; (void)use_esp;
	uprintf("Windows To Go is not supported on Linux");
	ErrorStatus = RUFUS_ERROR(ERROR_NOT_SUPPORTED);
	return FALSE;
}

/// <summary>
/// Add unattend.xml to 'sources\boot.wim' (install) or 'Windows\Panther\' (Windows To Go).
/// </summary>
/// <param name="drive_letter">The letter of the drive where the \sources\boot.wim image resides.</param>
/// <param name="flags">A bitmap of unattend flags to apply.</param>
/// <returns>TRUE on success, FALSE on error.</returns>
BOOL ApplyWindowsCustomization(const char* drive_name, int flags)
// NB: Work with a copy of unattend_xml_flags as a parameter since we will modify it.
{
	BOOL r = FALSE, update_boot_wim = FALSE;
	int i, wim_index = 2, wuc_index = 0, num_replaced = 0;
	const char* efi_ex_path = "Windows/Boot/EFI_EX";
	const char* fonts_ex_path = "Windows/Boot/Fonts_EX";
	char boot_wim_path[MAX_PATH];
	char tmp_path[2][MAX_PATH] = { "", "" };
	char appraiserres_dll_src[MAX_PATH], appraiserres_dll_dst[MAX_PATH];
	char setup_exe[MAX_PATH], setup_dll[MAX_PATH], md5sum_path[MAX_PATH];
	char path[MAX_PATH], *rep, *tmp_dir_end;
	uint8_t* buf = NULL;
	uint16_t setup_arch;
	uint32_t len;
	DWORD dwSize;
	FILE* fd_md5sum;
	WIMStruct* wim = NULL;
	struct wimlib_update_command wuc[2] = { 0 };

	static_sprintf(boot_wim_path, "%s/sources/boot.wim", drive_name);
	static_sprintf(appraiserres_dll_src, "%s/sources/appraiserres.dll", drive_name);
	static_sprintf(appraiserres_dll_dst, "%s/sources/appraiserres.bak", drive_name);
	static_sprintf(setup_exe, "%s/setup.exe", drive_name);
	static_sprintf(setup_dll, "%s/setup.dll", drive_name);
	static_sprintf(md5sum_path, "%s/md5sum.txt", drive_name);

	assert(unattend_xml_path != NULL);
	uprintf("Applying Windows customization:");
	PrintStatus(0, MSG_326);
	if (flags & UNATTEND_WINDOWS_TO_GO) {
		static_sprintf(path, "%s/Windows/Panther", drive_name);
		if (!CreateDirectoryA(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
			uprintf("Could not create '%s' : %s", path, WindowsErrorString());
			goto out;
		}
		static_sprintf(path, "%s/Windows/Panther/unattend.xml", drive_name);
		if (!CopyFileA(unattend_xml_path, path, TRUE)) {
			uprintf("Could not create '%s' : %s", path, WindowsErrorString());
			goto out;
		}
		uprintf("Added '%s'", path);
	} else {
		if (flags & UNATTEND_WINPE_SETUP_MASK) {
			// Create a backup of sources\appraiserres.dll and then create an empty file to
			// allow in-place upgrades without TPM/SB. Note that we need to create an empty,
			// appraiserres.dll otherwise setup.exe extracts its own.
			if (!MoveFileExU(appraiserres_dll_src, appraiserres_dll_dst, MOVEFILE_REPLACE_EXISTING)
				&& GetLastError() != ERROR_FILE_NOT_FOUND) {
				uprintf("Could not rename '%s': %s", appraiserres_dll_src, WindowsErrorString());
			} else {
				if (GetLastError() == ERROR_SUCCESS)
					uprintf("Renamed '%s' → '%s'", appraiserres_dll_src, appraiserres_dll_dst);
				CloseHandle(CreateFileU(appraiserres_dll_src, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
					NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
				uprintf("Created '%s' placeholder", appraiserres_dll_src);
				if (validate_md5sum) {
					md5sum_totalbytes -= _filesizeU(appraiserres_dll_dst);
					StrArrayAdd(&modified_files, appraiserres_dll_src, TRUE);
				}
			}
			// Apply the 'setup.exe' wrapper for Windows 11 24H2 in-place upgrades
			if (img_report.win_version.build >= 26000) {
				dwSize = read_file(setup_exe, &buf);
				if (dwSize != 0) {
					setup_arch = GetPeArch(buf);
					safe_free(buf);
					if (setup_arch != IMAGE_FILE_MACHINE_AMD64 && setup_arch != IMAGE_FILE_MACHINE_ARM64) {
						uprintf("WARNING: Unsupported arch 0x%x -- in-place upgrade wrapper will not be added", setup_arch);
					} else if (!MoveFileExU(setup_exe, setup_dll, 0)) {
						uprintf("Could not rename '%s': %s", setup_exe, WindowsErrorString());
					} else {
						uprintf("Renamed '%s' → '%s'", setup_exe, setup_dll);
						buf = GetResource(hMainInstance, MAKEINTRESOURCEA(setup_arch == IMAGE_FILE_MACHINE_AMD64 ? IDR_SETUP_X64 : IDR_SETUP_ARM64),
							_RT_RCDATA, "setup.exe", &dwSize, FALSE);
						if (buf == NULL) {
							uprintf("Could not access embedded 'setup.exe'");
						} else if (write_file(setup_exe, buf, dwSize) == dwSize) {
							uprintf("Created '%s' bypass wrapper (from embedded)", setup_exe);
							if (validate_md5sum) {
								if ((fd_md5sum = fopenU(md5sum_path, "ab")) != NULL) {
									fprintf(fd_md5sum, "00000000000000000000000000000000  ./setup.dll\n");
									fclose(fd_md5sum);
								}
								StrArrayAdd(&modified_files, setup_exe, TRUE);
								StrArrayAdd(&modified_files, setup_dll, TRUE);
								md5sum_totalbytes += dwSize;
							}
						} else {
							uprintf("Could not create '%s' bypass wrapper", setup_exe);
						}
						buf = NULL;
					}
				}
			}
		}

		UpdateProgressWithInfoForce(OP_PATCH, MSG_325, 0, PATCH_PROGRESS_TOTAL);
		// We only need to alter boot.wim if we have windowsPE data to deal with.
		// If not, we can just copy our unattend.xml in \sources\$OEM$\$$\Panther\.
		if (flags & UNATTEND_WINPE_SETUP_MASK || flags & UNATTEND_USE_MS2023_BOOTLOADERS) {
			if (validate_md5sum)
				md5sum_totalbytes -= _filesizeU(boot_wim_path);
			// Some "unofficial" ISOs have a modified boot.wim that doesn't have Windows Setup at index 2...
			// TODO: we could try to look for "Microsoft Windows Setup" in the XML DESCRIPTION to locate the index
			wimlib_global_init(0);
			wimlib_set_print_errors(true);
			update_boot_wim = (wimlib_open_wimU(boot_wim_path, WIMLIB_OPEN_FLAG_WRITE_ACCESS, &wim) == 0);
			if (!update_boot_wim) {
				uprintf("Could not open '%s'", boot_wim_path);
				goto out;
			}
			// Setup image should be index 2
			if (wimlib_resolve_image(wim, "2") != 2) {
				uprintf("WARNING: This image appears to be an UNOFFICIAL Windows ISO!");
				uprintf("Rufus recommends that you only use OFFICIAL retail Microsoft Windows images, such as");
				uprintf("the ones that can be downloaded through the download facility of this application.");
				wim_index = 1;
			}
		}

		// PORT: the LabConfig bypass keys can't be added to the offline registry
		// hive of boot.wim on Linux, so they are always created by the windowsPE
		// pass of unattend.xml (which is upstream's fallback for this case).
		if (flags & UNATTEND_SECUREBOOT_TPM_MINRAM)
			uprintf("Creating the TPM/Secure Boot/RAM bypass registry keys through unattend.xml");

	copy_unattend:
		if (flags & UNATTEND_WINPE_SETUP_MASK) {
			// If we have a windowsPE section, copy the answer files to the root of boot.wim as
			// Autounattend.xml. This also results in that file being automatically copied over
			// to %WINDIR%\Panther\unattend.xml for later passes processing.
			if_assert_fails(update_boot_wim)
				goto out;
			wuc[wuc_index].op = WIMLIB_UPDATE_OP_ADD;
			wuc[wuc_index].add.fs_source_path = safe_strdup(unattend_xml_path);
			wuc[wuc_index].add.wim_target_path = "Autounattend.xml";
			uprintf("Added '%s' to '%s'", wuc[wuc_index].add.wim_target_path, boot_wim_path);
			wuc_index++;
		} else {
			// If there is no windowsPE section in our unattend, then copying it as Autounattend.xml on
			// the root of boot.wim will not work as Windows Setup does *NOT* carry Autounattend.xml into
			// %WINDIR%\Panther\unattend.xml then (See: https://github.com/pbatard/rufus/issues/1981).
			// So instead, copy it to \sources\$OEM$\$$\Panther\unattend.xml on the media, as the content
			// of \sources\$OEM$\$$\* will get copied into %WINDIR%\ during the file copy phase.
			static_sprintf(path, "%s/sources/$OEM$/$$/Panther", drive_name);
			i = SHCreateDirectoryExU(NULL, path, NULL);
			if ((i != ERROR_SUCCESS) && (i != ERROR_ALREADY_EXISTS)) {
				SetLastError(i);
				uprintf("Error: Could not create directory '%s': %s", path, WindowsErrorString());
				goto out;
			}
			static_sprintf(path, "%s/sources/$OEM$/$$/Panther/unattend.xml", drive_name);
			if (!CopyFileU(unattend_xml_path, path, TRUE)) {
				uprintf("Could not create '%s': %s", path, WindowsErrorString());
				goto out;
			}
			uprintf("Created '%s'", path);
		}
		UpdateProgressWithInfoForce(OP_PATCH, MSG_325, 103, PATCH_PROGRESS_TOTAL);
	}

	if ((flags & UNATTEND_USE_MS2023_BOOTLOADERS) && !(flags & UNATTEND_WINDOWS_TO_GO)) {
		if_assert_fails(update_boot_wim)
			goto out;
		if (GetTempDirNameU(temp_dir, APPLICATION_NAME, 0, tmp_path[1]) == 0) {
			uprintf("WARNING: Could not create temp dir for 2023 signed UEFI bootloaders");
			goto out;
		}
		// If we have a '_EX' in the tmp name, we will have an issue
		if_assert_fails(strstr(tmp_path[1], "_EX") == NULL)
			goto out;
		// Extract the EFI_EX and Fonts_EX files
		if (wimlib_extract_pathsU(wim, wim_index, tmp_path[1], &efi_ex_path, 1,
				WIMLIB_EXTRACT_FLAG_NO_ACLS | WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE) != 0 ||
			wimlib_extract_pathsU(wim, wim_index, tmp_path[1], &fonts_ex_path, 1,
				WIMLIB_EXTRACT_FLAG_NO_ACLS | WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE) != 0) {
			uprintf("Could not extract 2023 signed UEFI bootloaders - Ignoring option");
		} else {
			StrArray files;
			len = (uint32_t)strlen(tmp_path[1]);
			tmp_dir_end = &tmp_path[1][len];

			// Copy/override the Font files
			static_strcat(tmp_path[1], "/Fonts_EX");
			StrArrayCreate(&files, 64);
			ListDirectoryContent(&files, tmp_path[1], LIST_DIR_TYPE_FILE);
			for (i = 0; i < (int)files.Index; i++) {
				static_sprintf(path, "%s/efi/microsoft/boot%s", drive_name, &files.String[i][len]);
				rep = remove_substr(path, "_EX");
				if (!CopyFileU(files.String[i], rep, FALSE))
					uprintf("WARNING: Could not copy '%s': %s", path, WindowsErrorString());
				else
					num_replaced++;
				safe_free(rep);
			}
			StrArrayDestroy(&files);

			// Replace /EFI/Boot/boot###.efi
			for (i = 1; i < ARRAYSIZE(efi_archname); i++) {
				*tmp_dir_end = '\0';
				static_strcat(tmp_path[1], "/EFI_EX/bootmgfw_EX.efi");
				static_sprintf(path, "%s/efi/boot/boot%s.efi", drive_name, efi_archname[i]);
				if (!PathFileExistsA(path))
					continue;
				if (!CopyFileU(tmp_path[1], path, FALSE))
					uprintf("WARNING: Could not replace 'boot%s.efi': %s", efi_archname[i], WindowsErrorString());
				else
					num_replaced++;
				break;
			}

			// Replace /bootmgr.efi
			*tmp_dir_end = '\0';
			static_strcat(tmp_path[1], "/EFI_EX/bootmgr_EX.efi");
			static_sprintf(path, "%s/bootmgr.efi", drive_name);
			if (!CopyFileU(tmp_path[1], path, FALSE))
				uprintf("WARNING: Could not replace 'bootmgr.efi': %s", WindowsErrorString());
			else
				num_replaced++;
			if (num_replaced != 0) {
				uprintf("Replaced %d EFI bootloader files with 'Windows UEFI CA 2023' compatible versions.", num_replaced);
				uprintf("Note that to boot this media, you must have a system where the 'Windows UEFI CA 2023'");
				uprintf("Secure Boot certificate has been installed.");
				uprintf("If needed, this can be accomplished using Mosby [https://github.com/pbatard/Mosby],");
				uprintf("which can be found, ready to use, in the UEFI Shell ISO images downloaded by Rufus.");
			}
			*tmp_dir_end = '\0';	// Else we won't be able to delete the temp dir
		}
	}

	r = TRUE;

out:
	if (update_boot_wim) {
		uprintf("Updating '%s[%d]'...", boot_wim_path, wim_index);
		if (wimlib_update_image(wim, wim_index, wuc, wuc_index, 0) != 0 ||
			wimlib_overwrite(wim, WIMLIB_WRITE_FLAG_RECOMPRESS, 0) != 0) {
			uprintf("Error: Failed to update %s", boot_wim_path);
			r = FALSE;
		}
		for (i = 0; i < ARRAYSIZE(tmp_path); i++)
			if (tmp_path[i][0])
				SHDeleteDirectoryExU(NULL, tmp_path[i], 0);
		for (i = 0; i < wuc_index; i++)
			free(wuc[i].add.fs_source_path);
		wimlib_free(wim);
		wimlib_global_cleanup();
		if (validate_md5sum) {
			md5sum_totalbytes += _filesizeU(boot_wim_path);
			StrArrayAdd(&modified_files, boot_wim_path, TRUE);
		}
		UpdateProgressWithInfo(OP_PATCH, MSG_325, PATCH_PROGRESS_TOTAL, PATCH_PROGRESS_TOTAL);
	}
	return r;
}
