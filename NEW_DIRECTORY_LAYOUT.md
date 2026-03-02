# The motivation

We should integrate uapmd-android project into this repository.
In the Android project, we create a symlink our source root directory
(which is currently our topdir) into its `app/src/main` as `cpp`.

But if we do so right now, adding uapmd-andoid topdir as `android` directory,
we end up creating a recursive directory up to the ancestor like
`android/app/src/main/cpp/android/app/src/main/cpp/android/app/src/main` ... 
So we have to first move the code-bearing topdir contents (e.g., `include`,
`src`, `tools`, `cmake`, `js`, `tests`, etc.) into `source` first. Non-code
assets such as `resources`, `docs`, and `flatpak` stay at the repository root
because packaging scripts and documentation already reference them there.

Then, having `source/src` is confusing, so we would restructure the
entire source tree from:

	include/remidy/xxx
	include/uapmd/xxx
	...
	src/remidy/xxx
	src/uapmd/xxx
	...
	tools/uapmd-app/xxx

to:

	source/remidy/include/remidy/xxx
	source/remidy/src/xxx
	source/uapmd/include/uapmd/xxx
	source/uapmd/src/xxx
	source/tools/remidy-scan/xxx (as is)
	source/tools/uapmd-app/xxx (as is)

then create a facade `include` directory with a handful of symlinks like:

	include/remidy       <- symlink to source/remidy/include
	include/uapmd        <- symlink to source/uapmd/include
	... (repeat for remidy-gui, remidy-tooling, uapmd-data, uapmd-engine, uapmd-file, etc.)

so that `#include <remidy/remidy.hpp>` etc. still works.

We have to rewrite the following dependent files:

- CMakeLists.txt and co.
- packaging scripts
- docs

Lastly, to make this repository itself ready for `CPMAddPackage` by other
project, create a top-level `CMakeLists.txt` that just include
`source/CMakeLists.txt` (which is equivalent to current `CMakeLists.txt`)
so far.
