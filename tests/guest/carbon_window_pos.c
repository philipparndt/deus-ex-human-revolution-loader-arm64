#include <Carbon/Carbon.h>
#include <stdio.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static void pr(WindowRef w, const char *tag) {
    HIRect c, s;
    HIWindowGetBounds(w, kWindowContentRgn, kHICoordSpace72DPIGlobal, &c);
    HIWindowGetBounds(w, kWindowStructureRgn, kHICoordSpace72DPIGlobal, &s);
    printf("%s: content %g,%g %gx%g structure %g,%g %gx%g\n", tag, c.origin.x, c.origin.y, c.size.width,
           c.size.height, s.origin.x, s.origin.y, s.size.width, s.size.height);
}
int main(void) {
    int attrs[] = {kHIWindowBitCompositing, kHIWindowBitStandardHandler, 0};
    HIRect r = {{0, 0}, {0, 0}};
    WindowRef w;
    HIWindowCreate(kDocumentWindowClass, attrs, NULL, kHICoordSpace72DPIGlobal, &r, &w);
    HIRect b = {{0, 52}, {1280, 720}};
    HIWindowSetBounds(w, kWindowContentRgn, kHICoordSpace72DPIGlobal, &b);
    pr(w, "after set 0,52");
    ShowWindow(w);
    pr(w, "after show");
    MoveWindowStructure(w, 0, 0);
    pr(w, "after move 0,0");
    RepositionWindow(w, w, kWindowCenterOnParentWindowScreen);
    pr(w, "after reposition");
    CGRect db = CGDisplayBounds(CGMainDisplayID());
    printf("display %g x %g\n", db.size.width, db.size.height);
    return 0;
}
