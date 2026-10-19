/*
 * Rufus: The Reliable USB Formatting Utility
 * PE executable parsing, SBAT and Secure Boot certificate lists
 * Copyright © 2012-2025 Pete Batard <pete@akeo.ie>
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
 * The PE/SBAT/thumbprint parsing functions come from upstream parser.c,
 * MachineToArch() from rufus.c, UseLocalDbx() from net.c, and the remote
 * Secure Boot data retrieval from stdlg.c (where upstream piggybacks it on
 * the Fido version check). GetIssuerCertificateInfo() (upstream pki.c, over
 * the Windows CryptoAPI) is reimplemented with OpenSSL's PKCS#7 support.
 */

#include "rfus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <openssl/evp.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

/* From upstream efi.h (which also defines GUID variables, so it can only be
 * included by a single compilation unit) */
typedef struct _WIN_CERTIFICATE {
	UINT32  dwLength;
	UINT16  wRevision;
	UINT16  wCertificateType;
} WIN_CERTIFICATE;
#define WIN_CERT_TYPE_PKCS_SIGNED_DATA	0x0002


char *sbat_level_txt = NULL, *sb_active_txt = NULL, *sb_revoked_txt = NULL;
sbat_entry_t* sbat_entries = NULL;
thumbprint_list_t *sb_active_certs = NULL, *sb_revoked_certs = NULL;

static BOOL utf16_name_matches(const uint16_t* utf16, const wchar_t* name)
{
	size_t i;
	for (i = 0; name[i] != 0; i++)
		if (utf16[i] != (uint16_t)name[i])
			return FALSE;
	return TRUE;
}

/*
 * Parse an sbat_level.txt file and returns an array of (product_name, min_version) tuples.
 * Array must be freed by caller.
 */
sbat_entry_t* GetSbatEntries(char* sbatlevel)
{
	BOOL eol, eof;
	char* version_str;
	uint32_t i, num_entries;
	sbat_entry_t* sbat_list;

	if (sbatlevel == NULL)
		return NULL;

	num_entries = 1;
	for (i = 0; sbatlevel[i] != '\0'; i++) {
		if (sbatlevel[i] == '\n')
			num_entries++;
		if (sbatlevel[i] == '\r')
			sbatlevel[i] = '\n';
	}

	sbat_list = calloc(num_entries + 1, sizeof(sbat_entry_t));
	if (sbat_list == NULL)
		return NULL;

	num_entries = 0;
	for (i = 0; sbatlevel[i] != '\0'; ) {
		// Eliminate blank lines
		if (sbatlevel[i] == '\n') {
			i++;
			continue;
		}
		// Eliminate comments
		if (sbatlevel[i] == '#') {
			while (sbatlevel[i] != '\n' && sbatlevel[i] != '\0')
				i++;
			continue;
		}
		sbat_list[num_entries].product = &sbatlevel[i];
		for (; sbatlevel[i] != ',' && sbatlevel[i] != '\0' && sbatlevel[i] != '\n'; i++);
		if (sbatlevel[i] == '\0' || sbatlevel[i] == '\n')
			break;
		sbatlevel[i++] = '\0';
		version_str = &sbatlevel[i];
		for (; sbatlevel[i] != ',' && sbatlevel[i] != '\0' && sbatlevel[i] != '\n'; i++);
		eol = (sbatlevel[i] == '\0' || sbatlevel[i] == '\n');
		eof = (sbatlevel[i] == '\0');
		sbatlevel[i] = '\0';
		if (!eof)
			i++;
		// Allow the provision of an hex version
		if (version_str[0] == '0' && version_str[1] == 'x')
			sbat_list[num_entries].version = strtoul(version_str, NULL, 16);
		else
			sbat_list[num_entries].version = strtoul(version_str, NULL, 10);
		if (!eol)
			for (; sbatlevel[i] != '\0' && sbatlevel[i] != '\n'; i++);
		if (sbat_list[num_entries].version != 0)
			num_entries++;
	}
	if (num_entries == 0) {
		free(sbat_list);
		return NULL;
	}

	return sbat_list;
}

/*
 * Parse a list of SHA-1 certificate hexascii thumbprints.
 * List must be freed by the caller.
 */
thumbprint_list_t* GetThumbprintEntries(char* thumbprints_txt)
{
	uint32_t i, j, num_entries;
	thumbprint_list_t* thumbprints;

	if (thumbprints_txt == NULL)
		return NULL;

	num_entries = 1;
	for (i = 0; thumbprints_txt[i] != '\0'; i++)
		if (thumbprints_txt[i] == '\n')
			num_entries++;

	thumbprints = calloc(sizeof(thumbprint_list_t) + num_entries * SHA1_HASHSIZE, 1);
	if (thumbprints == NULL)
		return NULL;
	thumbprints->count = 0;

	for (i = 0; thumbprints_txt[i] != '\0'; ) {
		// Eliminate blank lines
		if (thumbprints_txt[i] == '\n') {
			i++;
			continue;
		}
		// Eliminate lines that don't start by an hexadecimal digit
		if (!IS_HEXASCII(thumbprints_txt[i])) {
			while (thumbprints_txt[i] != '\n' && thumbprints_txt[i] != '\0')
				i++;
			continue;
		}
		for (j = 0; thumbprints_txt[i] != '\n' && thumbprints_txt[i] != '\0'; i++, j++) {
			if (!IS_HEXASCII(thumbprints_txt[i]))
				break;
			if ((j / 2) >= SHA1_HASHSIZE)
				break;
			thumbprints->list[thumbprints->count][j / 2] = thumbprints->list[thumbprints->count][j / 2] << 4;
			thumbprints->list[thumbprints->count][j / 2] |= FROM_HEXASCII(thumbprints_txt[i]);
			if (j == 2 * SHA1_HASHSIZE - 1)
				thumbprints->count++;
		}
		while (thumbprints_txt[i] != '\n' && thumbprints_txt[i] != '\0')
			i++;
	}

	if (thumbprints->count == 0) {
		free(thumbprints);
		return NULL;
	}

	return thumbprints;
}

/*
 * PE parsing functions
 */

// Return the arch of a PE executable buffer
uint16_t GetPeArch(uint8_t* buf)
{
	IMAGE_DOS_HEADER* dos_header = (IMAGE_DOS_HEADER*)buf;
	IMAGE_NT_HEADERS32* pe_header;

	if (buf == NULL || dos_header->e_magic != IMAGE_DOS_SIGNATURE)
		return IMAGE_FILE_MACHINE_UNKNOWN;

	pe_header = (IMAGE_NT_HEADERS32*)&buf[dos_header->e_lfanew];
	if (pe_header->Signature != IMAGE_NT_SIGNATURE)
		return IMAGE_FILE_MACHINE_UNKNOWN;
	return pe_header->FileHeader.Machine;
}

// Return the address and (optionally) the length of a PE section from a PE buffer
uint8_t* GetPeSection(uint8_t* buf, const char* name, uint32_t* len)
{
	char section_name[IMAGE_SIZEOF_SHORT_NAME] = { 0 };
	uint32_t i, nb_sections;
	IMAGE_DOS_HEADER* dos_header = (IMAGE_DOS_HEADER*)buf;
	IMAGE_NT_HEADERS32* pe_header;
	IMAGE_NT_HEADERS64* pe64_header;
	IMAGE_SECTION_HEADER* section_header;

	static_strcpy(section_name, name);

	if (buf == NULL || name == NULL || dos_header->e_magic != IMAGE_DOS_SIGNATURE)
		return NULL;

	pe_header = (IMAGE_NT_HEADERS32*)&buf[dos_header->e_lfanew];
	if (pe_header->Signature != IMAGE_NT_SIGNATURE)
		return NULL;
	if (pe_header->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 || pe_header->FileHeader.Machine == IMAGE_FILE_MACHINE_ARM) {
		section_header = (IMAGE_SECTION_HEADER*)(&pe_header[1]);
		nb_sections = pe_header->FileHeader.NumberOfSections;
	} else {
		pe64_header = (IMAGE_NT_HEADERS64*)pe_header;
		section_header = (IMAGE_SECTION_HEADER*)(&pe64_header[1]);
		nb_sections = pe64_header->FileHeader.NumberOfSections;
	}
	for (i = 0; i < nb_sections; i++) {
		if (memcmp(section_header[i].Name, section_name, sizeof(section_name)) == 0) {
			if (len != NULL)
				*len = section_header->SizeOfRawData;
			return &buf[section_header[i].PointerToRawData];
		}
	}
	return NULL;
}

// Convert an RVA address to a physical address from a PE buffer
uint8_t* RvaToPhysical(uint8_t* buf, uint32_t rva)
{
	uint32_t i, nb_sections;
	IMAGE_DOS_HEADER* dos_header = (IMAGE_DOS_HEADER*)buf;
	IMAGE_NT_HEADERS32* pe_header;
	IMAGE_NT_HEADERS64* pe64_header;
	IMAGE_SECTION_HEADER* section_header;

	if (buf == NULL || dos_header->e_magic != IMAGE_DOS_SIGNATURE)
		return NULL;

	pe_header = (IMAGE_NT_HEADERS32*)&buf[dos_header->e_lfanew];
	if (pe_header->Signature != IMAGE_NT_SIGNATURE)
		return NULL;
	if (pe_header->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 || pe_header->FileHeader.Machine == IMAGE_FILE_MACHINE_ARM) {
		section_header = (IMAGE_SECTION_HEADER*)(pe_header + 1);
		nb_sections = pe_header->FileHeader.NumberOfSections;
	} else {
		pe64_header = (IMAGE_NT_HEADERS64*)pe_header;
		section_header = (IMAGE_SECTION_HEADER*)(pe64_header + 1);
		nb_sections = pe64_header->FileHeader.NumberOfSections;
	}

	for (i = 0; i < nb_sections; i++) {
		if ((section_header[i].VirtualAddress <= rva && rva < (section_header[i].VirtualAddress + section_header[i].Misc.VirtualSize)))
			break;
	}
	if (i >= nb_sections)
		return NULL;

	return &buf[section_header[i].PointerToRawData + (rva - section_header[i].VirtualAddress)];
}

// Using the MS APIs to poke the resources of the EFI bootloaders is simply TOO. DAMN. SLOW.
// So, to QUICKLY access the resources we need, we reivent Microsoft's sub-optimal resource parser.
static BOOL FoundResourceRva = FALSE;
uint32_t FindResourceRva(const wchar_t* name, uint8_t* root, uint8_t* dir, uint32_t* len)
{
	uint32_t i, rva;
	IMAGE_RESOURCE_DIRECTORY* _dir = (IMAGE_RESOURCE_DIRECTORY*)dir;
	IMAGE_RESOURCE_DIRECTORY_ENTRY* dir_entry = (IMAGE_RESOURCE_DIRECTORY_ENTRY*)&_dir[1];
	IMAGE_RESOURCE_DIR_STRING_U* dir_string;
	IMAGE_RESOURCE_DATA_ENTRY* data_entry;

	if (root == NULL || dir == NULL || name == NULL)
		return 0;

	// Initial invocation should always start at the root
	if (root == dir)
		FoundResourceRva = FALSE;

	for (i = 0; i < (uint32_t)_dir->NumberOfNamedEntries + _dir->NumberOfIdEntries; i++) {
		if (!FoundResourceRva && i < _dir->NumberOfNamedEntries) {
			dir_string = (IMAGE_RESOURCE_DIR_STRING_U*)(root + dir_entry[i].NameOffset);
			// PORT: wchar_t is 32-bit on Linux whereas PE resource names are UTF-16
			if (dir_string->Length != wcslen(name) || !utf16_name_matches(dir_string->NameString, name))
				continue;
			FoundResourceRva = TRUE;
		}
		if (dir_entry[i].OffsetToData & IMAGE_RESOURCE_DATA_IS_DIRECTORY) {
			rva = FindResourceRva(name, root, &root[dir_entry[i].OffsetToDirectory], len);
			if (rva != 0)
				return rva;
		} else if (FoundResourceRva) {
			data_entry = (IMAGE_RESOURCE_DATA_ENTRY*)(root + dir_entry[i].OffsetToData);
			if (len != NULL)
				*len = data_entry->Size;
			return data_entry->OffsetToData;
		}
	}
	return 0;
}

uint8_t* GetPeSignatureData(uint8_t* buf)
{
	IMAGE_DOS_HEADER* dos_header = (IMAGE_DOS_HEADER*)buf;
	IMAGE_NT_HEADERS32* pe_header;
	IMAGE_NT_HEADERS64* pe64_header;
	IMAGE_DATA_DIRECTORY sec_dir;
	WIN_CERTIFICATE* cert;

	if (buf == NULL || dos_header->e_magic != IMAGE_DOS_SIGNATURE)
		return NULL;

	pe_header = (IMAGE_NT_HEADERS32*)&buf[dos_header->e_lfanew];
	if (pe_header->Signature != IMAGE_NT_SIGNATURE)
		return NULL;

	if (pe_header->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 || pe_header->FileHeader.Machine == IMAGE_FILE_MACHINE_ARM) {
		sec_dir = pe_header->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
	} else {
		pe64_header = (IMAGE_NT_HEADERS64*)pe_header;
		sec_dir = pe64_header->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
	}
	if (sec_dir.VirtualAddress == 0 || sec_dir.Size == 0)
		return NULL;

	cert = (WIN_CERTIFICATE*)&buf[sec_dir.VirtualAddress];
	if (cert->dwLength == 0 || cert->wCertificateType != WIN_CERT_TYPE_PKCS_SIGNED_DATA)
		return NULL;

	return (uint8_t*)cert;
}

enum ArchType MachineToArch(WORD machine)
{
	switch (machine) {
	case IMAGE_FILE_MACHINE_I386:
		return ARCH_X86_32;
	case IMAGE_FILE_MACHINE_AMD64:
		return ARCH_X86_64;
	case IMAGE_FILE_MACHINE_ARM:
		return ARCH_ARM_32;
	case IMAGE_FILE_MACHINE_ARM64:
		return ARCH_ARM_64;
	case IMAGE_FILE_MACHINE_IA64:
		return ARCH_IA_64;
	case IMAGE_FILE_MACHINE_RISCV64:
		return ARCH_RISCV_64;
	case IMAGE_FILE_MACHINE_LOONGARCH64:
		return ARCH_LOONGARCH_64;
	case IMAGE_FILE_MACHINE_EBC:
		return ARCH_EBC;
	default:
		return ARCH_UNKNOWN;
	}
}


/*
 * Get the latest SBAT level and Secure Boot certificate thumbprints from the
 * Rufus server (as upstream does at startup). Silently falls back to the
 * embedded lists when offline.
 */
void FetchSecureBootData(void)
{
	uint32_t i;
	uint64_t len;

	safe_free(sbat_entries);
	safe_free(sbat_level_txt);
	safe_free(sb_active_txt);
	safe_free(sb_revoked_txt);
	safe_free(sb_active_certs);
	safe_free(sb_revoked_certs);

	len = DownloadToFileOrBuffer(RUFUS_URL "/sbat_level.txt", NULL, (BYTE**)&sbat_level_txt, NULL, FALSE);
	if (len != 0 && len < 1 * KB) {
		sbat_entries = GetSbatEntries(sbat_level_txt);
		if (sbat_entries != NULL) {
			for (i = 0; sbat_entries[i].product != NULL; i++);
			if (i > 0)
				uprintf("Found %d additional UEFI revocation filters from remote SBAT", i);
		}
	}
	len = DownloadToFileOrBuffer(RUFUS_URL "/sb_active.txt", NULL, (BYTE**)&sb_active_txt, NULL, FALSE);
	if (len != 0 && len < 1 * KB) {
		sb_active_certs = GetThumbprintEntries(sb_active_txt);
		if (sb_active_certs != NULL)
			uprintf("Found %d active Secure Boot certificate entries from remote", sb_active_certs->count);
	}
	len = DownloadToFileOrBuffer(RUFUS_URL "/sb_revoked.txt", NULL, (BYTE**)&sb_revoked_txt, NULL, FALSE);
	if (len != 0 && len < 1 * KB) {
		sb_revoked_certs = GetThumbprintEntries(sb_revoked_txt);
		if (sb_revoked_certs != NULL)
			uprintf("Found %d revoked Secure Boot certificate entries from remote", sb_revoked_certs->count);
	}
}

/*
 * Get the name and SHA-1 thumbprint of the certificate that issued the
 * Authenticode signature of a PE (the 'WIN_CERTIFICATE' from the security
 * directory). Returns 2 if the issuer certificate was found, 1 if only the
 * signer certificate was, 0 on error, -1 on invalid parameters. Same as
 * upstream, which builds the chain from the certificates of the signature.
 */
int GetIssuerCertificateInfo(uint8_t* cert, cert_info_t* info)
{
	WIN_CERTIFICATE* pWinCert = (WIN_CERTIFICATE*)cert;
	const unsigned char* p;
	PKCS7* p7 = NULL;
	STACK_OF(X509)* certs;
	STACK_OF(PKCS7_SIGNER_INFO)* signers;
	PKCS7_SIGNER_INFO* si;
	X509 *signer = NULL, *issuer = NULL, *target;
	unsigned char* der = NULL;
	unsigned int md_len = SHA1_HASHSIZE;
	int i, der_len, ret = 0;

	if (info == NULL)
		return -1;
	if (pWinCert == NULL || pWinCert->dwLength <= sizeof(WIN_CERTIFICATE))
		return 0;

	p = (const unsigned char*)&pWinCert[1];
	p7 = d2i_PKCS7(NULL, &p, (long)(pWinCert->dwLength - sizeof(WIN_CERTIFICATE)));
	if (p7 == NULL || !PKCS7_type_is_signed(p7)) {
		uprintf("PKI: Failed to get signature");
		goto out;
	}
	certs = p7->d.sign->cert;
	signers = PKCS7_get_signer_info(p7);
	if (certs == NULL || signers == NULL || sk_PKCS7_SIGNER_INFO_num(signers) < 1) {
		uprintf("PKI: Failed to get signer info");
		goto out;
	}
	si = sk_PKCS7_SIGNER_INFO_value(signers, 0);
	/* Search for the signer certificate */
	for (i = 0; i < sk_X509_num(certs) && signer == NULL; i++) {
		X509* c = sk_X509_value(certs, i);
		if ((X509_NAME_cmp(X509_get_issuer_name(c), si->issuer_and_serial->issuer) == 0) &&
			(ASN1_INTEGER_cmp(X509_get_serialNumber(c), si->issuer_and_serial->serial) == 0))
			signer = c;
	}
	if (signer == NULL) {
		uprintf("PKI: Failed to locate signer certificate in store");
		goto out;
	}
	/* Look for the issuer (CA) certificate among the ones from the signature */
	for (i = 0; i < sk_X509_num(certs) && issuer == NULL; i++) {
		X509* c = sk_X509_value(certs, i);
		if ((c != signer) && (X509_check_issued(c, signer) == X509_V_OK))
			issuer = c;
	}
	target = (issuer != NULL) ? issuer : signer;

	/* Isolate the certificate subject (common) name */
	if (X509_NAME_get_text_by_NID(X509_get_subject_name(target), NID_commonName,
		info->name, sizeof(info->name)) <= 0) {
		uprintf("PKI: Failed to get Subject Name");
		goto out;
	}
	/* The thumbprint is the SHA-1 of the DER encoded certificate */
	der_len = i2d_X509(target, &der);
	if (der_len <= 0 || !EVP_Digest(der, (size_t)der_len, info->thumbprint, &md_len, EVP_sha1(), NULL)) {
		uprintf("PKI: Failed to compute the thumbprint");
		goto out;
	}
	ret = (issuer != NULL) ? 2 : 1;

out:
	OPENSSL_free(der);
	PKCS7_free(p7);
	return ret;
}
