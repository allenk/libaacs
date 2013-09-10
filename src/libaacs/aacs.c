/*
 * This file is part of libaacs
 * Copyright (C) 2009-2010  Obliter0n
 * Copyright (C) 2009-2010  npzacs
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include <util/attributes.h>

#include "aacs-version.h"
#include "aacs.h"
#include "crypto.h"
#include "mmc.h"
#include "mkb.h"
#include "file/file.h"
#include "file/keydbcfg.h"
#include "util/macro.h"
#include "util/logging.h"
#include "util/strutl.h"

#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#include <gcrypt.h>


struct aacs {
    /* current disc */
    char     *path;
    int       mkb_version;
    uint8_t   disc_id[20];

    /* VID is cached for BD-J */
    uint8_t   vid[16];
    /* PMSN is cached for BD-J */
    uint8_t   pmsn[16];

    /* unit key for each CPS unit */
    uint32_t  num_uks;
    uint8_t  *uks;

    /* CPS unit of currently selected title */
    uint16_t  current_cps_unit;

    /* title -> CPS unit mappings */
    uint32_t  num_titles;
    uint16_t *cps_units;  /* [0] = first play ; [1] = top menu ; [2] = title 1 ... */

    /* bus encryption */
    int       bee; // bus encryption enabled flag in content certificate
    int       bec; // bus encryption capable flag in drive certificate
    uint8_t   read_data_key[16];
};

static const uint8_t empty_key[] = "\x00\x00\x00\x00\x00\x00\x00\x00"
                                   "\x00\x00\x00\x00\x00\x00\x00\x00";

static int _validate_pk(const uint8_t *pk,
                        const uint8_t *cvalue, const uint8_t *uv, const uint8_t *vd,
                        uint8_t *mk)
{
    gcry_cipher_hd_t gcry_h;
    int a;
    uint8_t dec_vd[16];
    char str[40];

    DEBUG(DBG_AACS, "Validate processing key %s...\n", print_hex(str, pk, 16));
    DEBUG(DBG_AACS, " Using:\n");
    DEBUG(DBG_AACS, "   UV: %s\n", print_hex(str, uv, 4));
    DEBUG(DBG_AACS, "   cvalue: %s\n", print_hex(str, cvalue, 16));
    DEBUG(DBG_AACS, "   Verification data: %s\n", print_hex(str, vd, 16));

    gcry_cipher_open(&gcry_h, GCRY_CIPHER_AES, GCRY_CIPHER_MODE_ECB, 0);
    gcry_cipher_setkey(gcry_h, pk, 16);
    gcry_cipher_decrypt(gcry_h, mk, 16, cvalue, 16);

    for (a = 0; a < 4; a++) {
        mk[a + 12] ^= uv[a];
    }

    gcry_cipher_setkey(gcry_h, mk, 16);
    gcry_cipher_decrypt (gcry_h, dec_vd, 16, vd, 16);
    gcry_cipher_close(gcry_h);

    if (!memcmp(dec_vd, "\x01\x23\x45\x67\x89\xAB\xCD\xEF", 8)) {
        DEBUG(DBG_AACS, "Processing key %s is valid!\n", print_hex(str, pk, 16));
        return AACS_SUCCESS;
    }

    return AACS_ERROR_NO_PK;
}

static int _rl_verify_signature(const uint8_t *rl, size_t size)
{
    int    entries = MKINT_BE32(rl + 12 + 8);
    size_t len     = 12 + 12 + 8 * entries; /* type_and_version_rec=12, rl_header=12, rl=entries*8 */

    if (len + 40 > size) {
        DEBUG(DBG_AACS, "revocation list size mismatch\n");
        return 0;
    }

    return crypto_aacs_verify_aacsla(rl + len, rl, len);
}

static void _update_drl(MKB *mkb)
{
    uint32_t version = mkb_version(mkb);
    uint32_t cache_version;

    if (!cache_get("drl", &cache_version, NULL, NULL) || cache_version < version) {
        size_t drl_len;
        const uint8_t *drl_rec = mkb_drive_revokation_entries(mkb, &drl_len);
        const uint8_t *v_rec   = mkb_type_and_version_record(mkb);

        if (drl_rec && v_rec) {
            drl_rec -= 4;
            drl_len += 4;

            uint8_t *data = malloc(12 + drl_len);
            memcpy(data,      v_rec,   12);
            memcpy(data + 12, drl_rec, drl_len);
            if (!_rl_verify_signature(data, drl_len + 12)) {
                DEBUG(DBG_AACS | DBG_CRIT, "invalid drive revocation list signature, not using it\n");
            } else {
                cache_save("drl", version, data, drl_len + 12);
            }
            X_FREE(data);
        }
    }
}

static int _calc_mk(AACS *aacs, uint8_t *mk, pk_list *pkl)
{
    int a, num_uvs = 0;
    size_t len;
    uint8_t *buf = NULL;
    MKB *mkb = NULL;
    const uint8_t *rec, *uvs;

    /* Skip if retrieved from config file */
    if (memcmp(mk, empty_key, 16)) {
        return AACS_SUCCESS;
    }

    DEBUG(DBG_AACS, "Calculate media key...\n");

    if ((mkb = mkb_open(aacs->path))) {
        DEBUG(DBG_AACS, "Get UVS...\n");
        _update_drl(mkb);
        aacs->mkb_version = mkb_version(mkb);
        uvs = mkb_subdiff_records(mkb, &len);
        rec = uvs;
        while (rec < uvs + len) {
            if (rec[0] & 0xc0)
                break;
            rec += 5;
            num_uvs++;
        }

        DEBUG(DBG_AACS, "Get cvalues...\n");
        rec = mkb_cvalues(mkb, &len);

        for (; pkl && pkl->key; pkl = pkl->next) {
                uint8_t pk[16];
                hexstring_to_hex_array(pk, sizeof(pk), pkl->key);
                DEBUG(DBG_AACS, "Trying processing key...\n");

                for (a = 0; a < num_uvs; a++) {
                    if (AACS_SUCCESS == _validate_pk(pk, rec + a * 16, uvs + 1 + a * 5,
                      mkb_mk_dv(mkb), mk)) {
                        mkb_close(mkb);
                        X_FREE(buf);

                        char str[40];
                        DEBUG(DBG_AACS, "Media key: %s\n", print_hex(str, mk, 16));
                        return AACS_SUCCESS;
                    }
                }
            }

        mkb_close(mkb);
        X_FREE(buf);

        DEBUG(DBG_AACS | DBG_CRIT, "Error calculating media key. Missing right processing key ?\n");
        return AACS_ERROR_NO_PK;
    }

    DEBUG(DBG_AACS | DBG_CRIT, "Error opening %s/AACS/MKB_RO.inf\n", aacs->path);
    return AACS_ERROR_CORRUPTED_DISC;
}

static MKB *_get_hrl_mkb(MMC *mmc)
{
    MKB     *mkb = NULL;
    uint8_t *data;
    int      mkb_size;

    data = mmc_read_mkb(mmc, 0, &mkb_size);

    /* check acquired hrl signature */
    if (data && mkb_size > 0) {
        if (_rl_verify_signature(data, mkb_size)) {
            mkb = mkb_init(data, mkb_size);
            DEBUG(DBG_AACS, "Partial hrl mkb read. Version: %d\n", mkb_version(mkb));
        } else {
            DEBUG(DBG_AACS | DBG_CRIT, "invalid host revocation list signature, not using it\n");
            X_FREE(data);
        }
    }

    if (mkb) {
        /* use latest version, keep cache up-to-date */
        uint32_t size;
        size = mkb_data_size(mkb);
        data = cache_get_or_update("hrl", mkb_data(mkb), &size, mkb_version(mkb));

        if (!_rl_verify_signature(data, size)) {
            DEBUG(DBG_AACS | DBG_CRIT, "invalid cached revocation list signature, replacing it\n");
            cache_save("hrl", mkb_version(mkb), mkb_data(mkb), mkb_data_size(mkb));
            X_FREE(data);
        } else {
            /* use cached version */
            mkb_close(mkb);
            mkb = mkb_init(data, size);
        }

    } else {
        /* use cached version */
        uint32_t size;
        data = cache_get_or_update("hrl", NULL, &size, 0);
        if (data && size > 0) {
            if (!_rl_verify_signature(data, size)) {
                mkb = mkb_init(data, size);
            } else {
                DEBUG(DBG_AACS | DBG_CRIT, "invalid cached revocation list signature, deleting cache\n");
                cache_remove("hrl");
            }
        }
    }


    if (mkb) {
        DEBUG(DBG_AACS, "Using hrl version %d\n", mkb_version(mkb));
    }

    return mkb;
}

static int _read_vid(AACS *aacs, cert_list *hcl, int force_mmc)
{
    /* Use VID given in config file if available */
    if (!force_mmc && memcmp(aacs->vid, empty_key, 16)) {
        return AACS_SUCCESS;
    }

    MMC* mmc = NULL;
    if (!(mmc = mmc_open(aacs->path))) {
        return AACS_ERROR_MMC_OPEN;
    }

    int error_code = AACS_ERROR_NO_CERT;

    MKB *hrl_mkb = _get_hrl_mkb(mmc);

    for (;hcl && hcl->host_priv_key && hcl->host_cert; hcl = hcl->next) {

        char tmp_str[2*92+1];
        uint8_t priv_key[20], cert[92];
        hexstring_to_hex_array(priv_key, sizeof(priv_key), hcl->host_priv_key);
        hexstring_to_hex_array(cert,     sizeof(cert),     hcl->host_cert);

        if (!crypto_aacs_verify_host_cert(cert)) {
            DEBUG(DBG_AACS, "Not using invalid host certificate %s.\n",
                  print_hex(tmp_str, cert, 92));
            continue;
        }

        if (mkb_host_cert_is_revoked(hrl_mkb, cert + 4)) {
            DEBUG(DBG_AACS | DBG_CRIT, "Host certificate %s has been revoked.\n",
                  print_hex(tmp_str, cert + 4, 6));
            error_code = AACS_ERROR_CERT_REVOKED;
            //continue;
        }

        DEBUG(DBG_AACS, "Trying host certificate (id 0x%s)...\n",
              print_hex(tmp_str, cert + 4, 6));

        int mmc_result = mmc_read_vid(mmc, priv_key, cert, aacs->vid, aacs->pmsn);
        switch (mmc_result) {
            case MMC_SUCCESS:
                mkb_close(hrl_mkb);
                mmc_close(mmc);
                /* cache vid */
                keycache_save("vid", aacs->disc_id, aacs->vid, 16);
                return AACS_SUCCESS;
            case MMC_ERROR_CERT_REVOKED:
                error_code = AACS_ERROR_CERT_REVOKED;
                break;
            case MMC_ERROR:
            default:
                error_code = AACS_ERROR_MMC_FAILURE;
                break;
        }
    }

    mkb_close(hrl_mkb);
    mmc_close(mmc);

    DEBUG(DBG_AACS, "Error reading VID!\n");
    return error_code;
}

static int _read_read_data_key(AACS *aacs, cert_list *hcl)
{
    MMC* mmc = NULL;
    if (!(mmc = mmc_open(aacs->path))) {
        return AACS_ERROR_MMC_OPEN;
    }

    int error_code = AACS_ERROR_NO_CERT;

    for (;hcl && hcl->host_priv_key && hcl->host_cert; hcl = hcl->next) {

        char tmp_str[2*92+1];
        uint8_t priv_key[20], cert[92];
        hexstring_to_hex_array(priv_key, sizeof(priv_key), hcl->host_priv_key);
        hexstring_to_hex_array(cert,     sizeof(cert),     hcl->host_cert);

        if (!crypto_aacs_verify_host_cert(cert)) {
            DEBUG(DBG_AACS, "Not using invalid host certificate %s.\n",
                  print_hex(tmp_str, cert, 92));
            continue;
        }

        DEBUG(DBG_AACS, "Trying host certificate (id 0x%s)...\n",
              print_hex(tmp_str, cert + 4, 6));

        int mmc_result = mmc_read_data_keys(mmc, priv_key, cert, aacs->read_data_key, NULL);

        switch (mmc_result) {
            case MMC_SUCCESS:
                mmc_close(mmc);
                return AACS_SUCCESS;
            case MMC_ERROR_CERT_REVOKED:
                error_code = AACS_ERROR_CERT_REVOKED;
                break;
            case MMC_ERROR:
            default:
                error_code = AACS_ERROR_MMC_FAILURE;
                break;
        }
    }

    mmc_close(mmc);

    DEBUG(DBG_AACS, "Error reading read data key!\n");
    return error_code;
}

static int _calc_vuk(AACS *aacs, uint8_t *mk, uint8_t *vuk,
                     pk_list *pkl, cert_list *host_cert_list)
{
    int error_code;

    /* Skip if retrieved from config file */
    if (memcmp(vuk, empty_key, 16)) {
        DEBUG(DBG_AACS, "Using VUK from config file\n");
        return AACS_SUCCESS;
    }

    /* get cached vuk */
    if (keycache_find("vuk", aacs->disc_id, vuk, 16)) {
        DEBUG(DBG_AACS, "Using cached VUK\n");
        return AACS_SUCCESS;
    }

    /* make sure we have media key */
    error_code = _calc_mk(aacs, mk, pkl);
    if (error_code != AACS_SUCCESS) {
        return error_code;
    }

    /* acquire VID */
    error_code = _read_vid(aacs, host_cert_list, 0);
    if (error_code != AACS_SUCCESS) {
        return error_code;
    }

    int a;
    gcry_cipher_hd_t gcry_h;

    gcry_cipher_open(&gcry_h, GCRY_CIPHER_AES, GCRY_CIPHER_MODE_ECB, 0);
    gcry_cipher_setkey(gcry_h, mk, 16);
    gcry_cipher_decrypt(gcry_h, vuk, 16, aacs->vid, 16);
    gcry_cipher_close(gcry_h);

    for (a = 0; a < 16; a++) {
        vuk[a] ^= aacs->vid[a];
    }

    char str[40];
    DEBUG(DBG_AACS, "Volume unique key: %s\n", print_hex(str, vuk, 16));

    /* cache vuk */
    keycache_save("vuk", aacs->disc_id, vuk, 16);

    return AACS_SUCCESS;
}

static uint16_t _read_u16(AACS_FILE_H *fp)
{
  uint8_t data[2] = {0, 0};

  file_read(fp, data, sizeof(uint16_t));

  return MKINT_BE16(data);
}

static void _read_uks_map(AACS *aacs, AACS_FILE_H *fp)
{
    uint16_t first_play, top_menu;
    unsigned i;

    DEBUG(DBG_AACS, "Assigning CPS units to titles ...\n");

    X_FREE(aacs->cps_units);
    aacs->current_cps_unit = 0;

    file_seek(fp, 16 + 4, SEEK_SET);

    first_play = _read_u16(fp);
    top_menu   = _read_u16(fp);

    DEBUG(DBG_AACS, "Title FP : CPS unit %d\n", first_play);
    DEBUG(DBG_AACS, "Title TM : CPS unit %d\n", top_menu);

    aacs->num_titles   = _read_u16(fp);
    aacs->cps_units    = calloc(sizeof(uint16_t), aacs->num_titles + 2);
    aacs->cps_units[0] = first_play;
    aacs->cps_units[1] = top_menu;

    for (i = 2; i < aacs->num_titles + 2; i++) {
        _read_u16(fp); /* reserved */
        aacs->cps_units[i] = _read_u16(fp);
        DEBUG(DBG_AACS, "Title %02d : CPS unit %d\n", i - 1, aacs->cps_units[i]);
    }

    /* validate */
    for (i = 0; i < aacs->num_titles + 2; i++) {
        if (aacs->cps_units[i])
            aacs->cps_units[i]--; /* number [1...N] --> index [0...N-1] */
        if (aacs->cps_units[i] >= aacs->num_uks) {
            DEBUG(DBG_AACS, " *** Invalid CPS unit for title %d: %d !\n", (int) i - 1, aacs->cps_units[i]);
            aacs->cps_units[i] = 0;
        }
    }
}

static AACS_FILE_H *_open_unit_key_file(const char *path)
{
    AACS_FILE_H *fp;
    char        *f_name;

    f_name = str_printf("%s/AACS/Unit_Key_RO.inf", path);
    fp = file_open(f_name, "rb");

    if (!fp) {
        DEBUG(DBG_AACS | DBG_CRIT, "Error opening unit key file %s\n", f_name);
    }

    X_FREE(f_name);
    return fp;
}

static AACS_FILE_H *_open_content_certificate_file(const char *path)
{
    AACS_FILE_H *fp;
    char        *f_name;

    f_name = str_printf("%s/AACS/Content000.cer", path);
    fp = file_open(f_name, "rb");

    if (!fp) {
        DEBUG(DBG_AACS | DBG_CRIT, "Error opening content certificate file %s\n", f_name);
    }

    X_FREE(f_name);
    return fp;
}

/* Function that collects keys from keydb config entry */
static void _find_config_entry(AACS *aacs, title_entry_list *ce,
                               uint8_t *mk, uint8_t *vuk)
{
    uint8_t discid[20];
    char str[48];

    aacs->uks = NULL;
    aacs->num_uks = 0;

        while (ce && ce->entry.discid) {
            memset(discid, 0, sizeof(discid));
            hexstring_to_hex_array(discid, sizeof(discid),
                                   ce->entry.discid);
            if (!memcmp(aacs->disc_id, discid, 20)) {
                DEBUG(DBG_AACS, "Found config entry for discid %s\n",
                      ce->entry.discid);
                break;
            }

            ce = ce->next;
        }
        if (!ce) {
            return;
        }

        if (ce->entry.mek) {
            hexstring_to_hex_array(mk, 16, ce->entry.mek);

            DEBUG(DBG_AACS, "Found media key for %s: %s\n",
                  ce->entry.discid, print_hex(str, mk, 16));
        }

        if (ce->entry.vid) {
            hexstring_to_hex_array(aacs->vid, sizeof(aacs->vid),
                                    ce->entry.vid);

            DEBUG(DBG_AACS, "Found volume id for %s: %s\n",
                  ce->entry.discid, print_hex(str, aacs->vid, 16));
        }

        if (ce->entry.vuk) {
            hexstring_to_hex_array(vuk, 16, ce->entry.vuk);

            DEBUG(DBG_AACS, "Found volume unique key for %s: %s\n",
                  ce->entry.discid, print_hex(str, vuk, 16));
        }

        if (ce->entry.uk) {
            DEBUG(DBG_AACS, "Acquire CPS unit keys from keydb config file...\n");

            digit_key_pair_list *ukcursor = ce->entry.uk;
            while (ukcursor && ukcursor->key_pair.key) {
                aacs->num_uks++;

                aacs->uks = (uint8_t*)realloc(aacs->uks, 16 * aacs->num_uks);
                hexstring_to_hex_array(aacs->uks + (16 * (aacs->num_uks - 1)), 16,
                                      ukcursor->key_pair.key);

                DEBUG(DBG_AACS, "Unit key %d from keydb entry: %s\n",
                      aacs->num_uks,
                      print_hex(str, aacs->uks + (16 * (aacs->num_uks - 1)), 16));

                ukcursor = ukcursor->next;
            }
        }
}

static int _calc_uks(AACS *aacs, config_file *cf)
{
    AACS_FILE_H *fp = NULL;
    uint8_t  buf[16];
    uint64_t f_pos;
    unsigned int i;
    int error_code;

    uint8_t mk[16] = {0}, vuk[16] = {0};

    DEBUG(DBG_AACS, "Searching for keydb config entry...\n");
    _find_config_entry(aacs, cf->list, mk, vuk);

    /* Skip if retrieved from config file */
    if (aacs->uks) {
        return AACS_SUCCESS;
    }

    /* Make sure we have VUK */
    error_code = _calc_vuk(aacs, mk, vuk, cf->pkl, cf->host_cert_list);
    if (error_code != AACS_SUCCESS) {
        return error_code;
    }

    DEBUG(DBG_AACS, "Calculate CPS unit keys...\n");

    fp = _open_unit_key_file(aacs->path);
    if (!fp) {
        return AACS_ERROR_CORRUPTED_DISC;
    }

    if ((file_read(fp, buf, 4)) == 4) {
        f_pos = MKINT_BE32(buf);

        // Read number of keys
        file_seek(fp, f_pos, SEEK_SET);
        if ((file_read(fp, buf, 2)) == 2) {
            aacs->num_uks = MKINT_BE16(buf);

            X_FREE(aacs->uks);
            aacs->uks = calloc(aacs->num_uks, 16);

            DEBUG(DBG_AACS, "%d CPS unit keys\n", aacs->num_uks);

        } else {
            aacs->num_uks = 0;
            DEBUG(DBG_AACS | DBG_CRIT, "Error reading number of unit keys\n");
            error_code = AACS_ERROR_CORRUPTED_DISC;
        }

        // Read keys
        for (i = 0; i < aacs->num_uks; i++) {
            f_pos += 48;

            file_seek(fp, f_pos, SEEK_SET);
            if ((file_read(fp, buf, 16)) != 16) {
                DEBUG(DBG_AACS, "Unit key %d: read error\n", i);
                aacs->num_uks = i;
                error_code = AACS_ERROR_CORRUPTED_DISC;
                break;
            }

            gcry_cipher_hd_t gcry_h;
            gcry_cipher_open(&gcry_h, GCRY_CIPHER_AES,
                             GCRY_CIPHER_MODE_ECB, 0);
            gcry_cipher_setkey(gcry_h, vuk, 16);
            gcry_cipher_decrypt(gcry_h, aacs->uks + 16*i, 16, buf, 16);
            gcry_cipher_close(gcry_h);

            char str[40];
            DEBUG(DBG_AACS, "Unit key %d: %s\n", i,
                  print_hex(str, aacs->uks + 16*i, 16));
        }

        /* failing next is not fatal, it just slows down things */
        _read_uks_map(aacs, fp);

        file_close(fp);

        return error_code;
    }

    file_close(fp);

    DEBUG(DBG_AACS | DBG_CRIT, "Error reading unit keys\n");
    return AACS_ERROR_CORRUPTED_DISC;
}

static int _calc_title_hash(const char *path, uint8_t *title_hash)
{
    AACS_FILE_H *fp = NULL;
    uint8_t *ukf_buf;
    char     str[48];
    int64_t  f_size;
    int      result = AACS_SUCCESS;

    fp = _open_unit_key_file(path);
    if (!fp) {
        return AACS_ERROR_CORRUPTED_DISC;
    }

    file_seek(fp, 0, SEEK_END);
    f_size = file_tell(fp);
    file_seek(fp, 0, SEEK_SET);

    ukf_buf = malloc(f_size);

    if ((file_read(fp, ukf_buf, f_size)) == f_size) {
        crypto_aacs_title_hash(ukf_buf, f_size, title_hash);
        DEBUG(DBG_AACS, "Disc ID: %s\n", print_hex(str, title_hash, 20));

    } else {
        result = AACS_ERROR_CORRUPTED_DISC;
        DEBUG(DBG_AACS | DBG_CRIT, "Failed to read %"PRIu64" bytes from unit key file %s/AACS/Unit_Key_RO.inf", f_size, path);
    }

    file_close(fp);
    X_FREE(ukf_buf);

    return result;
}

static int _get_bus_encryption_enabled(const char *path)
{
    AACS_FILE_H *fp = NULL;
    uint8_t buf[2];

    fp = _open_content_certificate_file(path);
    if (!fp) {
        DEBUG(DBG_AACS | DBG_CRIT, "Unable to open content certificate\n");
        return 0;
    }

    if (file_read(fp, buf, 2) == 2) {
        int bee = (buf[1] & 0x80) >> 7;
        DEBUG(DBG_AACS, "Bus Encryption Enabled flag in content certificate: %d\n", bee);
        file_close(fp);
        return bee;
    } else {
        DEBUG(DBG_AACS | DBG_CRIT, "Failed to read 2 bytes from content certificate file\n");
        file_close(fp);
        return 0;
    }
}

static int _get_bus_encryption_capable(const char *path)
{
    MMC* mmc = NULL;
    uint8_t drive_cert[92];

    if (!(mmc = mmc_open(path))) {
        return 0;
    }

    if (mmc_read_drive_cert(mmc, drive_cert) == MMC_SUCCESS) {
        int bec = drive_cert[1] & 1;
        mmc_close(mmc);
        DEBUG(DBG_AACS, "Bus Encryption Capable flag in drive certificate: %d\n", bec);
        return bec;
    } else {
        DEBUG(DBG_AACS | DBG_CRIT, "Unable to read drive certificate\n");
        mmc_close(mmc);
        return 0;
    }
}

static int _verify_ts(uint8_t *buf, size_t size)
{
    uint8_t *ptr;

    if (size < 192) {
        return 1;
    }

    for (ptr=buf; ptr < buf+192; ptr++) {
        int failed = 0;
        if (*ptr == 0x47) {
            uint8_t *ptr2;

            for (ptr2=ptr; ptr2 < buf + size; ptr2 += 192) {
                if (*ptr2 != 0x47) {
                    failed = 1;
                    break;
                }
            }
            if (!failed) {
                return 1;
            }
        }
        ptr++;
    }

    DEBUG(DBG_AACS, "Failed to verify TS!\n");

    return 0;
}

#define ALIGNED_UNIT_LEN 6144
static int _decrypt_unit(AACS *aacs, uint8_t *out_buf, const uint8_t *in_buf, uint32_t curr_uk)
{
    gcry_cipher_hd_t gcry_h;
    int a;
    uint8_t key[16], iv[] = "\x0b\xa0\xf8\xdd\xfe\xa6\x1f\xb3"
                            "\xd8\xdf\x9f\x56\x6a\x05\x0f\x78";

    gcry_cipher_open(&gcry_h, GCRY_CIPHER_AES, GCRY_CIPHER_MODE_ECB, 0);
    gcry_cipher_setkey(gcry_h, aacs->uks + curr_uk * 16, 16);
    gcry_cipher_encrypt(gcry_h, key, 16, in_buf, 16);
    gcry_cipher_close(gcry_h);

    for (a = 0; a < 16; a++) {
        key[a] ^= in_buf[a];
    }

    memcpy(out_buf, in_buf, 16); /* first 16 bytes are plain */

    gcry_cipher_open(&gcry_h, GCRY_CIPHER_AES, GCRY_CIPHER_MODE_CBC, 0);
    gcry_cipher_setkey(gcry_h, key, 16);
    gcry_cipher_setiv(gcry_h, iv, 16);
    gcry_cipher_decrypt(gcry_h, out_buf + 16, ALIGNED_UNIT_LEN - 16, in_buf + 16, ALIGNED_UNIT_LEN - 16);
    gcry_cipher_close(gcry_h);

    if (_verify_ts(out_buf, ALIGNED_UNIT_LEN)) {
        return 1;
    }

    if (curr_uk < aacs->num_uks - 1) {
        return _decrypt_unit(aacs, out_buf, in_buf, curr_uk++);
    }

    return 0;
}

#define SECTOR_LEN 2048
static void _decrypt_bus(AACS *aacs, uint8_t *out_buf, const uint8_t *in_buf)
{
    gcry_cipher_hd_t gcry_h;
    uint8_t iv[] = "\x0b\xa0\xf8\xdd\xfe\xa6\x1f\xb3"
                   "\xd8\xdf\x9f\x56\x6a\x05\x0f\x78";

    memcpy(out_buf, in_buf, 16); /* first 16 bytes are plain */

    gcry_cipher_open(&gcry_h, GCRY_CIPHER_AES, GCRY_CIPHER_MODE_CBC, 0);
    gcry_cipher_setkey(gcry_h, aacs->read_data_key, 16);
    gcry_cipher_setiv(gcry_h, iv, 16);
    gcry_cipher_decrypt(gcry_h, out_buf + 16, SECTOR_LEN - 16, in_buf + 16, SECTOR_LEN - 16);
    gcry_cipher_close(gcry_h);
}

void aacs_get_version(int *major, int *minor, int *micro)
{
    *major = AACS_VERSION_MAJOR;
    *minor = AACS_VERSION_MINOR;
    *micro = AACS_VERSION_MICRO;
}

/* aacs_open2() wrapper for backwards compability */
AACS *aacs_open(const char *path, const char *configfile_path)
{
    int error_code;
    AACS *aacs;

    aacs = aacs_open2(path, configfile_path, &error_code);
    if (error_code == AACS_SUCCESS) {
        return aacs;
    }

    aacs_close(aacs);
    return NULL;
}

AACS *aacs_open2(const char *path, const char *configfile_path, int *error_code)
{
    DEBUG(DBG_AACS, "libaacs "AACS_VERSION_STRING" [%zd]\n", sizeof(AACS));

    DEBUG(DBG_AACS, "Initializing libgcrypt...\n");
    if (!crypto_init()) {
        DEBUG(DBG_AACS | DBG_CRIT, "Failed to initialize libgcrypt\n");
        return NULL;
    }

    AACS *aacs = calloc(1, sizeof(AACS));
    config_file *cf;

    aacs->path = str_printf("%s", path);

    *error_code = _calc_title_hash(path, aacs->disc_id);
    if (*error_code != AACS_SUCCESS) {
        aacs_close(aacs);
        return NULL;
    }

    cf = keydbcfg_config_load(configfile_path);
    if (!cf) {
        *error_code = AACS_ERROR_NO_CONFIG;
        aacs_close(aacs);
        return NULL;
    }

    DEBUG(DBG_AACS, "Starting AACS waterfall...\n");
    *error_code = _calc_uks(aacs, cf);
    if (*error_code != AACS_SUCCESS) {
        DEBUG(DBG_AACS, "Failed to initialize AACS!\n");
        keydbcfg_config_file_close(cf);
        aacs_close(aacs);
        return NULL;
    }

    aacs->bee = _get_bus_encryption_enabled(path);
    aacs->bec = _get_bus_encryption_capable(path);

    if (aacs->bee && aacs->bec) {
        *error_code = _read_read_data_key(aacs, cf->host_cert_list);
        if (*error_code != AACS_SUCCESS) {
            DEBUG(DBG_AACS | DBG_CRIT, "Unable to initialize bus encryption required by drive and disc\n");
            keydbcfg_config_file_close(cf);
            aacs_close(aacs);
            return NULL;
        }
    }

    keydbcfg_config_file_close(cf);

    DEBUG(DBG_AACS, "AACS initialized!\n");

    return aacs;
}

void aacs_close(AACS *aacs)
{
    if (!aacs)
        return;

    X_FREE(aacs->uks);
    X_FREE(aacs->cps_units);
    X_FREE(aacs->path);

    DEBUG(DBG_AACS, "AACS destroyed!\n");

    X_FREE(aacs);
}

int aacs_decrypt_unit(AACS *aacs, uint8_t *buf)
{
    uint8_t out_buf[ALIGNED_UNIT_LEN];
    int i;

    if (!(buf[0] & 0xc0)) {
        // TP_extra_header Copy_permission_indicator == 0, unit is not encrypted
        return 1;
    }

    if (aacs->bee && aacs->bec) {
        for (i = 0; i < ALIGNED_UNIT_LEN; i += SECTOR_LEN) {
            _decrypt_bus(aacs, out_buf + i, buf + i);
        }
        memcpy(buf, out_buf, ALIGNED_UNIT_LEN);
    }

    if (_decrypt_unit(aacs, out_buf, buf, aacs->current_cps_unit)) {
        memcpy(buf, out_buf, ALIGNED_UNIT_LEN);

        // Clear copy_permission_indicator bits
        for (i = 0; i < 6144; i += 192) {
            buf[i] &= ~0xc0;
        }

        return 1;
    }

    DEBUG(DBG_AACS, "Failed decrypting unit [6144 bytes]\n");

    return 0;
}

int aacs_get_mkb_version(AACS *aacs)
{
    if (!aacs->mkb_version) {
        MKB *mkb;
        if ((mkb = mkb_open(aacs->path))) {
            aacs->mkb_version = mkb_version(mkb);
            mkb_close(mkb);
        }
    }
    return aacs->mkb_version;
}

const uint8_t *aacs_get_disc_id(AACS *aacs)
{
    return aacs->disc_id;
}

const uint8_t *aacs_get_vid(AACS *aacs)
{
    if (!memcmp(aacs->vid, empty_key, 16)) {
        /* get cached vid */
        if (keycache_find("vid", aacs->disc_id, aacs->vid, 16)) {
            DEBUG(DBG_AACS, "Using cached VID\n");
            return aacs->vid;
        }

        config_file *cf = keydbcfg_config_load(NULL);
        if (cf) {
            _read_vid(aacs, cf->host_cert_list, 0);

            keydbcfg_config_file_close(cf);
        }
    }

    if (!memcmp(aacs->vid, empty_key, 16)) {
        DEBUG(DBG_AACS | DBG_CRIT, "aacs_get_vid() failed\n");
        return NULL;
    }

    return aacs->vid;
}

const uint8_t *aacs_get_pmsn(AACS *aacs)
{
    if (!memcmp(aacs->pmsn, empty_key, 16)) {
        config_file *cf = keydbcfg_config_load(NULL);
        if (cf) {
            _read_vid(aacs, cf->host_cert_list, 1);

            keydbcfg_config_file_close(cf);
        }
    }

    if (!memcmp(aacs->pmsn, empty_key, 16)) {
        DEBUG(DBG_AACS, "aacs_get_pmsn() failed\n");
        return NULL;
    }

    return aacs->pmsn;
}

static AACS_RL_ENTRY *_get_rl(const char *type, int *num_records, int *mkbv)
{
    uint32_t len, version;
    void *data = NULL;

    *num_records = *mkbv = 0;

    cache_get(type, &version, &len, NULL);

    if (version > 0 && len > 24) {
        data = malloc(len);
        if (cache_get(type, &version, &len, data) && len > 24) {

            if (_rl_verify_signature(data, len)) {
                *mkbv = version;
                *num_records = MKINT_BE32((uint8_t*)data + 20);
                memmove(data, (uint8_t*)data + 24, len - 24);

                int ii;
                AACS_RL_ENTRY *rl = data;
                for (ii = 0; ii < *num_records; ii++) {
                    rl[ii].range = MKINT_BE16((uint8_t*)&rl[ii].range);
                }

                return rl;
            }

            DEBUG(DBG_AACS | DBG_CRIT, "invalid signature in cached %s\n", type);
        }

        X_FREE(data);
    }

    return data;
}

AACS_RL_ENTRY *aacs_get_hrl(int *num_records, int *mkbv)
{
    return _get_rl("hrl", num_records, mkbv);
}

AACS_RL_ENTRY *aacs_get_drl(int *num_records, int *mkbv)
{
    return _get_rl("drl", num_records, mkbv);
}

void aacs_select_title(AACS *aacs, uint32_t title)
{
    if (!aacs) {
        return;
    }

    if (!aacs->cps_units) {
        DEBUG(DBG_AACS|DBG_CRIT, "aacs_select_title(): CPS units not read !\n");
        return;
    }

    if (title == 0xffff) {
        /* first play */
        aacs->current_cps_unit = aacs->cps_units[0];
        DEBUG(DBG_AACS, "aacs_set_title(first_play): CPS unit %d\n", aacs->current_cps_unit);
        return;
    }

    if (title <= aacs->num_titles) {
        aacs->current_cps_unit = aacs->cps_units[title + 1];
        DEBUG(DBG_AACS, "aacs_set_title(%d): CPS unit %d\n", title, aacs->current_cps_unit);
        return;
    }

    DEBUG(DBG_AACS|DBG_CRIT, "aacs_set_title(%d): invalid title !\n", title);
}
