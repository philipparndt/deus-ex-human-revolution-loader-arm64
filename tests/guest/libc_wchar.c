// setlocale/localeconv and the wide-character functions the guest libstdc++ needs.
#include <limits.h>
#include <locale.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>
#include "libc_check.h"

_Static_assert(sizeof(struct lconv) == 56 && offsetof(struct lconv, int_frac_digits) == 40, "lconv");
_Static_assert(sizeof(wchar_t) == 4 && sizeof(mbstate_t) == 128, "wchar");

int main(void) {
    const char *l = setlocale(LC_ALL, NULL);
    CHECK(l && strcmp(l, "C") == 0, "setlocale query %s", l ? l : "(null)");
    CHECK(setlocale(LC_ALL, "C") == setlocale(LC_ALL, NULL), "setlocale strings are stable");
    struct lconv *lc = localeconv();
    CHECK(lc && strcmp(lc->decimal_point, ".") == 0 && lc->thousands_sep && lc->int_frac_digits == CHAR_MAX, "localeconv");

    const wchar_t *w = L"Hello, wide world";
    CHECK(wcslen(w) == 17, "wcslen");
    CHECK(wcscmp(w, L"Hello") > 0 && wcsncmp(w, L"Hello", 5) == 0 && wcscoll(L"a", L"b") < 0, "wcscmp");
    wchar_t buf[64];
    CHECK(wmemcpy(buf, w, 18) == buf && wmemcmp(buf, w, 18) == 0, "wmemcpy/wmemcmp");
    CHECK(wmemmove(buf + 1, buf, 5) == buf + 1 && buf[1] == L'H', "wmemmove");
    CHECK(wmemset(buf, L'x', 3) == buf && buf[2] == L'x', "wmemset");
    CHECK(wmemchr(w, L'w', 17) == w + 7 && wmemchr(w, L'Z', 17) == NULL, "wmemchr");
    CHECK(wcschr(w, L',') == w + 5 && wcsstr(w, L"world") == w + 12, "wcschr/wcsstr");
    wchar_t xf[64];
    size_t xn = wcsxfrm(xf, L"abc", 64);
    CHECK(xn > 0 && xn < 64, "wcsxfrm");
    CHECK(btowc('A') == L'A' && wctob(L'z') == 'z' && btowc(EOF) == WEOF, "btowc/wctob");
    mbstate_t st;
    memset(&st, 0, sizeof st);
    wchar_t wc = 0;
    CHECK(mbrtowc(&wc, "Q", 1, &st) == 1 && wc == L'Q', "mbrtowc");
    char mb[8];
    CHECK(wcrtomb(mb, L'R', &st) == 1 && mb[0] == 'R', "wcrtomb");
    const char *src = "abc";
    memset(&st, 0, sizeof st);
    CHECK(mbsrtowcs(buf, &src, 64, &st) == 3 && src == NULL && wcscmp(buf, L"abc") == 0, "mbsrtowcs");
    const wchar_t *wsrc = L"xyz";
    char out[8];
    CHECK(wcsrtombs(out, &wsrc, sizeof out, &st) == 3 && wsrc == NULL && strcmp(out, "xyz") == 0, "wcsrtombs");
    wsrc = L"truncated";
    CHECK(wcsrtombs(out, &wsrc, 4, &st) == 4 && wsrc != NULL && wcscmp(wsrc, L"cated") == 0, "wcsrtombs partial");
    CHECK(iswctype(L'a', wctype("alpha")) && !iswctype(L'1', wctype("alpha")) && towupper(L'a') == L'A' && towlower(L'B') == L'b', "wctype");

    time_t t = 1234567890;
    struct tm tm;
    gmtime_r(&t, &tm);
    wchar_t tb[64];
    CHECK(wcsftime(tb, 64, L"%Y-%m-%d", &tm) == 10 && wcscmp(tb, L"2009-02-13") == 0, "wcsftime");
    int a = 0;
    float f = 0;
    char word[16];
    wchar_t wword[16];
    CHECK(swscanf(L"42 2.5 abc def", L"%d %f %15s %15ls", &a, &f, word, wword) == 4 && a == 42 && f == 2.5f &&
              strcmp(word, "abc") == 0 && wcscmp(wword, L"def") == 0,
          "swscanf");
    DONE();
}
