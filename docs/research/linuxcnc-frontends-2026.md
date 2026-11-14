# LinuxCNC frontend landscape, November 2026

This preserves the initial research that led to CoffeeCNC. The project owner's objection was visual and workflow quality, not simply whether an interface uses Qt, GTK or a browser. Changing the toolkit does not by itself produce a modern operator experience.

## Established approaches

| Project | Approach and relevance | Primary sources |
|---|---|---|
| Gmoccapy | Established LinuxCNC operator interface and the starting point for the discussion. Its existing arrangement did not meet the desired visual direction. | [LinuxCNC manual](https://linuxcnc.org/docs/stable/html/gui/gmoccapy.html) |
| Probe Basic | QtPyVCP-based milling interface, useful for studying practical tooling, probing and setup coverage. A stronger workflow reference than a reason to copy its styling. | [Project documentation](https://kcjengr.github.io/probe_basic/index.html), [QtPyVCP showcase](https://www.qtpyvcp.com/showcase/mill_vcps.html) |
| QtDragon / QtDragon HD | LinuxCNC QtVCP interfaces with integrated milling functions. Useful as examples of controller and setup coverage. | [LinuxCNC QtDragon manual](https://www.linuxcnc.org/docs/stable/html/gui/qtdragon.html) |
| Flex GUI | A configurable PyQt6 approach using Qt Designer and widget conventions. It gives developers building blocks; it is not a finished CoffeeCNC-style product design. | [Repository](https://github.com/jethornton/flexgui), [releases](https://github.com/jethornton/flexgui/releases) |

The Qt ecosystem was still evolving during this research. Consult the installation instructions for the actual selected branch/distribution rather than combining older PyQt5 instructions with newer Qt6 development documentation. [QtPyVCP development installation](https://www.qtpyvcp.com/install/dev_install.html)

## Newer browser-based work found in 2026

| Project | What was found | Maturity qualification |
|---|---|---|
| [LCNC Suite](https://github.com/bildobodo/lcnc-suite) | Vue 3/TypeScript frontend with a Python/FastAPI backend and WebSocket communication; [announced in April 2026](https://www.forum.linuxcnc.org/41-guis/58669-lcnc-suite-web-ui-for-linuxcnc). | The author describes unfinished work and cautions against production use. AI-assisted development and an extensive feature list do not establish validation on real machines. |
| [lcncgui](https://github.com/salomom/lcncgui) | Browser frontend with a Python backend; [introduced in July 2026](https://www.forum.linuxcnc.org/41-guis/59008-lcncgui-simple-web-based-linuxcnc-gui). Work includes tools, macros and physical-control/HAL integration. | The [changelog](https://github.com/salomom/lcncgui/blob/master/CHANGELOG.md) has a 1.0.0 entry dated July 19; the author's subsequent discussion still described work in progress. A version label alone is insufficient evidence of maturity. |
| [plasmaweb](https://github.com/RubenAhlhaus/plasmaweb) | An lcncgui-derived browser screen for QtPlasmaC, [announced in October 2026](https://www.forum.linuxcnc.org/41-guis/59366-plasmaweb-a-browser-based-qtplasmac-screen-for-linuxcnc). | Plasma-specific and described as simulation-tested by its author at announcement. It is not a validated milling frontend. |

These projects show that browser interfaces and AI-assisted development are appearing in the LinuxCNC ecosystem. None was established by this research as a ready-made replacement satisfying the requested visual direction and milling workflows. No comparative reliability tests were conducted.

## Elle and the architectural decision

The project's starting experience is [Elle](https://github.com/tinic/el-linuxcnc-electron), which moved from its earlier Electron structure to a C++ core with a GTK/WebKit-hosted interface. The owner reports that this simplification has worked well. CoffeeCNC adopts those architectural concepts as an independent product; it is not a milling tab bolted onto Elle.

C++ directly fits LinuxCNC's native NML boundary. However, LinuxCNC also documents and supports a Python interface; “Python is a bad fit for LinuxCNC” is too broad a conclusion. Rust's tradeoff here is the binding/integration work and available project experience, not an inherent inability to render or control a frontend. [LinuxCNC API documentation](https://linuxcnc.org/docs/stable/html/config/python-interface.html)

## Product conclusion

CoffeeCNC's opportunity is a coherent milling interface: clear machine state, practical manual/setup work, dependable program execution, deliberate recovery and optional richer job context. The [workflow research](machinist-workflows.md) and [sample corpus](../../samples/README.md) are now more useful inputs to that design than another round of toolkit comparisons.
