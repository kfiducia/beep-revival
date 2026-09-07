/*
 * plist-be-probe.c — big-endian correctness probe for libplist's binary-plist
 * (bplist00) integer serialization, as used by shairport-sync's AirPlay-2 SETUP
 * responses (plist_new_uint -> plist_to_bin).
 *
 * WHY THIS EXISTS
 * ---------------
 * Apple binary plists store multi-byte integers BIG-ENDIAN on the wire. libplist
 * 2.4.0 (the version OpenWrt 24.10 ships) makes every be16toh/be32toh/be64toh an
 * identity on a big-endian host ONLY IF the preprocessor symbol __BIG_ENDIAN__ is
 * defined in config.h. That symbol is set by ./configure via AC_C_BIGENDIAN. If the
 * cross-compile mis-detected endianness (or config.h wasn't included), every integer,
 * offset-table entry and trailer field in the plist gets an extra byte-swap, so the
 * bytes shairport hands the Mac in SETUP #2 are little-endian garbage and the Mac
 * silently aborts. This probe tells us, ON THE DEVICE, which case we're in.
 *
 * CRITICAL SUBTLETY — a round-trip PASS does NOT clear libplist.
 * ------------------------------------------------------------
 * plist_to_bin then plist_from_bin on the SAME host use the SAME (possibly wrong)
 * swap in both directions, so the value ALWAYS round-trips back to itself even when
 * the on-wire bytes are byte-swapped. The ONLY reliable discriminator is the raw
 * byte order of the serialized output, which is what this probe checks and prints.
 *
 * WHAT "CORRECT" LOOKS LIKE
 * -------------------------
 * plist_new_uint(0x0123456789ABCDEF) has length 16 (val > INT_MAX), so it serializes
 * via write_uint(): marker 0x14, then 8 zero bytes, then the 8 value bytes. On a
 * correctly-built big-endian libplist those 8 value bytes are, in order:
 *      01 23 45 67 89 AB CD EF     <-- ENDIAN-CORRECT (big-endian, what Apple expects)
 * A double-swapped (buggy) build emits them reversed:
 *      EF CD AB 89 67 45 23 01     <-- CORRUPTED (little-endian on the wire)
 *
 * BUILD (cross-compile against the OpenWrt 24.10 staging sysroot)
 * --------------------------------------------------------------
 * Adjust the two *_DIR globs to your tree (mips_24kc, musl, GCC version will vary):
 *
 *   OW=/build/openwrt
 *   TOOLCHAIN=$(ls -d $OW/staging_dir/toolchain-mips_24kc_gcc-*_musl)
 *   SYSROOT=$(ls -d $OW/staging_dir/target-mips_24kc_musl)
 *   export PATH="$TOOLCHAIN/bin:$PATH"
 *   export STAGING_DIR="$OW/staging_dir"
 *   mips-openwrt-linux-musl-gcc -O2 -static \
 *       -I"$SYSROOT/usr/include" \
 *       docs/probes/plist-be-probe.c \
 *       -L"$SYSROOT/usr/lib" -lplist-2.0 \
 *       -o /tmp/plist-be-probe
 *
 *   (Drop -static and scp the matching libplist-2.0.so.* alongside if you'd rather
 *    test the exact shared object the firmware links. -static tests the library code,
 *    which is what we care about for the endian question.)
 *
 * RUN (on the AR9331 device)
 * --------------------------
 *   scp -O /tmp/plist-be-probe root@<device>:/tmp/
 *   ssh root@<device> /tmp/plist-be-probe ; echo "exit=$?"
 *
 * Exit code: 0 = ENDIAN-CORRECT, 1 = CORRUPTED (byte-swapped), 2 = round-trip failed
 * (a different, worse failure), 3 = usage/alloc error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <plist/plist.h>

/* Report what the PROBE's own compiler thinks the byte order is. NOTE: this reflects
 * the flags THIS file was built with, not libplist's config.h. It's a sanity check
 * that we really are on a big-endian target; the authoritative signal is the hexdump
 * below, which reflects how libplist itself was compiled. */
static void print_probe_byte_order(void) {
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__)
    printf("probe TU __BYTE_ORDER__ big-endian? %s\n",
           (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) ? "yes" : "no");
#else
    printf("probe TU __BYTE_ORDER__ not exposed by this compiler\n");
#endif
#ifdef __BIG_ENDIAN__
    printf("probe TU __BIG_ENDIAN__ defined: yes\n");
#else
    printf("probe TU __BIG_ENDIAN__ defined: no\n");
#endif
    /* Runtime endianness check — always accurate for the running CPU. */
    uint32_t one = 1;
    printf("probe RUNTIME endianness: %s\n",
           (*(uint8_t *)&one == 1) ? "little-endian" : "big-endian");
}

static void hexdump(const char *label, const uint8_t *p, uint32_t n) {
    printf("%s (%u bytes):\n", label, n);
    for (uint32_t i = 0; i < n; i++) {
        printf("%02X ", p[i]);
        if ((i & 15) == 15) printf("\n");
    }
    if (n & 15) printf("\n");
}

/* Does the contiguous byte sequence `needle` appear anywhere in `hay`? */
static int contains(const uint8_t *hay, uint32_t hn, const uint8_t *needle, uint32_t nn) {
    if (nn == 0 || hn < nn) return 0;
    for (uint32_t i = 0; i + nn <= hn; i++)
        if (memcmp(hay + i, needle, nn) == 0) return 1;
    return 0;
}

int main(void) {
    const uint64_t TESTVAL = 0x0123456789ABCDEFull;
    const uint8_t be_seq[8] = { 0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF }; /* correct   */
    const uint8_t le_seq[8] = { 0xEF,0xCD,0xAB,0x89,0x67,0x45,0x23,0x01 }; /* corrupted */

    print_probe_byte_order();
    printf("\ntest value: 0x%016llX\n\n", (unsigned long long)TESTVAL);

    /* ---- serialize ---- */
    plist_t node = plist_new_uint(TESTVAL);
    if (!node) { fprintf(stderr, "plist_new_uint failed\n"); return 3; }

    char *bin = NULL;
    uint32_t binlen = 0;
    plist_to_bin(node, &bin, &binlen);
    if (!bin || binlen == 0) { fprintf(stderr, "plist_to_bin failed\n"); plist_free(node); return 3; }

    hexdump("plist_to_bin output", (uint8_t *)bin, binlen);

    int has_be = contains((uint8_t *)bin, binlen, be_seq, 8);
    int has_le = contains((uint8_t *)bin, binlen, le_seq, 8);

    printf("\nbig-endian value bytes (01 23 45 67 89 AB CD EF) present:  %s\n", has_be ? "YES" : "no");
    printf("swapped   value bytes (EF CD AB 89 67 45 23 01) present:  %s\n", has_le ? "YES" : "no");

    /* ---- round-trip (informational only; passes on the same host either way) ---- */
    plist_t back = NULL;
    plist_from_bin(bin, binlen, &back);
    uint64_t got = 0;
    int rt_ok = 0;
    if (back) {
        plist_get_uint_val(back, &got);
        rt_ok = (got == TESTVAL);
        plist_free(back);
    }
    printf("\nround-trip plist_get_uint_val: 0x%016llX  (%s)\n",
           (unsigned long long)got, rt_ok ? "value survived" : "VALUE LOST");
    printf("  NOTE: a surviving round-trip does NOT prove correctness — same-host\n"
           "  encode+decode cancel any consistent swap. Trust the byte order above.\n");

    free(bin);
    plist_free(node);

    /* ---- verdict ---- */
    printf("\n==== VERDICT ====\n");
    if (has_be && !has_le) {
        printf("PASS: libplist emits big-endian integers on this host. bplist is\n"
               "      endian-correct; the SETUP #2 plist is NOT corrupted by libplist.\n");
        return 0;
    }
    if (has_le && !has_be) {
        printf("FAIL: libplist double-swapped the integer (little-endian on the wire).\n"
               "      config.h almost certainly lacks __BIG_ENDIAN__ -> the SETUP #2\n"
               "      response IS corrupted and the Mac aborts. Rebuild libplist so\n"
               "      AC_C_BIGENDIAN detects big-endian (see BE-AP2-LIBPLIST.md).\n");
        return 1;
    }
    printf("INCONCLUSIVE: neither expected byte sequence found (binlen=%u). If the\n"
           "  round-trip also failed, libplist is broken in some other way — capture\n"
           "  the hexdump above.\n", binlen);
    return rt_ok ? 3 : 2;
}
