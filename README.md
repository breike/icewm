[icewm -- read me first file.  2025-03-09]: #

Ice Window Manager (IceWM)
==========================

IceWM is a window manager for the X Window System. The features of IceWM are
speed, simplicity, and not getting in the user's way.

> The name was decided on a very hot day... (and Marko started writing it in
> winter ;-)  The aim of IceWM is to have good 'Feel' and decent 'Look'. 'Feel'
> is much more important than 'Look' ...

This is a fork of the IceWM CVS on [sourceforge][12].  It includes all changes
from the `icewm-1-3-BRANCH` branch, greatly enhanced EWMH/ICCCM compliance, as
well as patches collected from Arch Linux, Debian, pld-linux, the IceWM bug
list, and various other GitHub forks.


Release
-------

This is the `icewm-4.1.0` package, released 2026-08-06.  This release, and
the latest version, can be obtained from [GitHub][1], using a command such as:

    $> git clone https://github.com/bbidulock/icewm.git

Please see the [NEWS][3] file for release notes and history of user visible
changes for the current version, and the [ChangeLog][4] file for a more
detailed history of implementation changes.  The [TODO][5] file lists features
not yet implemented and other outstanding items.

Please see the [INSTALL][7] file for installation instructions.

When working from `git(1)`, please use this file.  An abbreviated
installation procedure that works for most applications appears below.

This release is published under LGPL.  Please see the license
in the file [COPYING][9].


Quick Start
-----------

The quickest and easiest way to get icewm up and running is to run the
following commands:

    $> git clone https://github.com/bbidulock/icewm.git
    $> cd icewm
    $> ./autogen.sh
    $> ./configure
    $> make
    $> make DESTDIR="$pkgdir" install

This will configure, compile and install icewm the quickest.  For those who
like to spend the extra 15 seconds reading `./configure --help`, some compile
time options can be turned on and off before the build.

For general information on GNU's `./configure`, see the file [INSTALL][7].
To disable sound support, use --without-icesound. When the image library
supports SVG natively, you can use --disable-librsvg and --disable-nanosvg.

Please see the [INSTALL][7] file for more detailed installation instructions.
An alternative way to build IceWM using CMake is [documented here][19].
The [ChangeLog][4] file contains a detailed history of implementation changes.
The [COMPLIANCE][6] file lists the current state of EWMH/ICCCM compliance.  The
[NEWS][3] file has release notes and history of user visible changes of the
current version.  The [TODO][5] file lists features not yet implemented and
other outstanding items.

This release is published under LGPL license that can be found in the file
[COPYING][9].

Prerequisites
-------------

Building from tarball requires:

 - gcc or clang
 - imlib2 or libgdkpixbuf
 - libxcomposite
 - libxdamage
 - libxfixes
 - libxft
 - libxinerama
 - libxpm
 - libxrandr
 - libxrender

Building from git also requires:

 - complete autoconf or cmake toolchain
 - either markdown or asciidoctor
 - pod2man

For optional features:

  - libjpeg, libpng, librsvg or nanosvg
  - gettext, libfribidi

Configuring IceWM
-----------------

Documentation for configuring the window manager can be obtained from [IceWM
Website][13] or from the [online manual][15].
Since version 1.4.3 a complete and up-to-date set of manual pages is provided.
Use [__icewm__(1)][26] as a starting point.


Included Utilities
------------------

Currently, the only included utilities are:

 - [__icesh__(1)][25] (_a versatile window manipulation tool_),
 - [__icewmbg__(1)][22] (_a background setting program_),
 - [__icewm-session__(1)][27] (_a program to launch the window manager, icewmbg and
   icewmtray in an orderly fashion_),
 - [__icewm-menu-fdo__(1)][24] (_a utility to genenerate XDG menus_),
 - [__icewmhint__(1)][23] (_a utility to set IceWM-specific window options hint_).
 - [__icesound__(1)][21] (_play audio files when interesting GUI events happen_).


Tiling and flexible frames
--------------------------

IceWM can arrange windows in two complementary, opt-in ways, both driven
through `icesh(1)` and both per-workspace:

 * **Binary tiling** (_herbstluftwm_ style): windows live in a tree of
   frames that is split and reorganised; the tree splits the whole work
   area. Enable it with `icesh tiling` (bare command toggles tiling off
   and on).

 * **Flexible frames**: independent, explicitly positioned rectangles
   that may overlap; windows bound to a frame are resized to it. Frames
   can be grouped, and a group can be brought up again with one command.

All commands take effect on the active workspace and are documented in
`man icesh`.

**Binary tiling** commands:

    icesh tiling                         # toggle tiling on this workspace
    icesh tiling split [v|h [frac]]      # split focused frame (v = left/right, h = top/bottom; default 0.5)
    icesh tiling remove                  # merge focused frame into its sibling
    icesh tiling focus <path-or-label>   # focus frame at path (e.g. 01) or by label
    icesh tiling setlabel <label>        # label the focused frame (empty clears)
    icesh tiling dump                    # print the layout as an S-expression
    icesh tiling load <layout>           # replace the layout from a dump

**Flexible-frame** commands:

    icesh flex add <label> <x> <y> [w [h]]   # create/update a frame; binds the focused window
    icesh flex remove <label>                # delete a frame, unbinding its windows
    icesh flex focus <label>                 # focus the window bound to this frame
    icesh flex setlabel <label> <new>        # rename a frame
    icesh flex resize <label> <dw> <dh> [dx [dy]]  # grow/shrink (and shift) a frame; "." = focused
    icesh flex move <label> <dx> <dy>              # shift a frame without changing size; "." = focused
    icesh flex bind <label>                        # move the focused window into this frame
    icesh flex dump                          # list frames as "label x y w h"
    icesh flex clear                         # remove all frames of this workspace
    icesh flex highlight pen <n>             # set focus-outline thickness, 0..255 (default 3)
    icesh flex highlight color <r> <g> <b>   # set focus-outline color, 0..255 each (default 0x30 0xFF 0x60)

**Flexible-frame groups:**

    icesh flex group add <name> <labels...>  # add frames to a group (creating it)
    icesh flex group open <name>             # activate the group's last focused frame
    icesh flex group focus [next|prev] [name]# cycle focus within a group, or across all frames
    icesh flex group remove <name>           # delete the group (frames are kept)
    icesh flex group dump                    # list groups as "name label1 label2 ..."

Groups are a purely organizational layer on top of the frames: the
frames keep their own geometry and membership in several groups at
once. `flex group open` brings up the frame of the group that was
focused most recently (falling back to the first focusable member);
`flex group focus` wraps around and optional `prev` cycles backwards.

`flex resize` changes a frame's geometry by relative deltas instead of
recreating it like `flex add` does: the frame keeps its identity, bound
windows and group membership, and only its rectangle (and everything
inside it) moves and resizes. A label of `.` applies to the focused
frame, and the assigned `focus` outline follows the frame:

    icesh flex resize . -20 0    # shrink the focused frame by 20 px
    icesh flex resize A 0 0 10 10  # move frame A 10 px right and down
    icesh flex move A 10 10      # move frame A 10 px right and down (size unchanged)

`flex bind` moves the focused window into another frame: it is rebound
even if it already lived in a frame (its group membership is kept),
and the layout is reapplied so the window snaps to the target frame's
rectangle:

    icesh flex focus A && icesh flex bind B   # move the window focused on A into B

While a flexible frame is focused, a persistent outline is drawn around
the frame rectangle so you can always see which frame is active. It
stays visible on non-fullscreen windows (it is hidden while the focused
window is fullscreen, and follows the frame when focus moves); its
thickness and color are adjustable at runtime:

    icesh flex highlight pen 5              # thicker outline
    icesh flex highlight color 255 128 0    # orange outline

Third-party Utilities
---------------------

Unspecified keyboard shortcuts can be handled with the __bbkeys__(1) utility
available from [GitHub][16].

XDG compliant menus may be generated using the __xde-menu__(1) utility
available from [GitHub][20].

For additional utilities see the [IceWM FAQ][14].


Bug Reports
-----------

Issues can be reported on [GitHub][2].  Please try to submit short patches or
pull requests if you can.  If you would like to perform regular maintenance
activities (e.g. if you are a maintainer of an IceWM package for a
distribution), contact me for push access.

I normally like to have the issuers of problem reports close the report once
it has been resolved.  I do not want you to think that we are being dismissive,
because I welcome all reports.

Bug reports, feedback, and suggestions pertaining to the original CVS version
can be sent to: Marko.Macek@gmx.net or icewm-user@lists.sourceforge.net

See also [BUGS][8], [TODO][5] and the sites at:

  - https://ice-wm.org/
  - https://sourceforge.net/projects/icewm/


Development
-----------

If you would like to develop against this fork, the easiest way is to obtain a
[GitHub account][10], fork the [repository][1] and perform your development.
Send me a pull request when you have something stable.  If you submit regular
pull requests that get accepted, I will just give to push access to save time.


Translations
------------

You can provide translations by using the [openSUSE weblate tool][11].
There are two XDG files,
[icewm.desktop][17] and [icewm-session.desktop][18] which may need manual
translations.  If you have difficulties using the tools, just send me the updated
`.po` file or a patch to apply.


[1]: https://github.com/bbidulock/icewm
[2]: https://github.com/ice-wm/icewm/issues
[3]: https://github.com/ice-wm/icewm/blob/master/NEWS
[4]: https://github.com/ice-wm/icewm/blob/master/ChangeLog
[5]: https://github.com/ice-wm/icewm/blob/master/TODO
[6]: https://github.com/ice-wm/icewm/blob/master/COMPLIANCE
[7]: https://github.com/ice-wm/icewm/blob/master/INSTALL
[8]: https://github.com/ice-wm/icewm/blob/master/BUGS
[9]: https://github.com/ice-wm/icewm/blob/master/COPYING
[10]: https://github.com/
[11]: https://l10n.opensuse.org/projects/icewm/icewm-1-4-branch/
[12]: https://sourceforge.net/projects/icewm/
[13]: https://ice-wm.org/
[14]: https://ice-wm.org/FAQ/
[15]: https://ice-wm.org/manual/
[16]: https://github.com/bbidulock/bbkeys/
[17]: https://github.com/ice-wm/icewm/blob/master/lib/icewm.desktop
[18]: https://github.com/ice-wm/icewm/blob/master/lib/icewm-session.desktop
[19]: https://github.com/ice-wm/icewm/blob/master/INSTALL-cmakebuild.md
[20]: https://github.com/ice-wm/xde-menu/
[21]: https://ice-wm.org/man/icesound
[22]: https://ice-wm.org/man/icewmbg
[23]: https://ice-wm.org/man/icewmhint
[24]: https://ice-wm.org/man/icewm-menu-fdo
[25]: https://ice-wm.org/man/icesh
[26]: https://ice-wm.org/man/icewm
[27]: https://ice-wm.org/man/icewm-session

[ vim: set ft=markdown sw=4 tw=80 nocin nosi fo+=tcqlorn spell: ]: #
