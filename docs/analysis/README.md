# Game binary analysis (not in the repository)

The generators in `tools/` and `src/guest/libstdcxx/check-exports.sh` read
listings derived from the game binary. They are not published; create them from
your own installed copy (`APP=".../Deus Ex Human Revolution - Directors Cut.app"`):

| File | Content | How |
| --- | --- | --- |
| `hr-imports.tsv` | `<library>\t<symbol>` for every imported symbol | `dyld_info -imports "$APP/Contents/MacOS/"*` (or `nm -m -u`) |
| `hr-selectors.txt` | Objective-C selectors the game sends | `otool -oV`, `__objc_selrefs` / `__message_refs` |
| `hr-classrefs.txt` | SDK classes the game references | `otool -oV`, `__cls_refs` |
| `hr-objc.txt` | the game's own ObjC classes and methods | `otool -oV` |
| `fmod-imports.txt`, `quincykit-imports.txt` | imports of bundled libraries | `nm -m -u` on the dylib / framework |
