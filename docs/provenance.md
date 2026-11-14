# Material provenance

This repository preserves the design discussion that led to CoffeeCNC, beginning 2026-11-14.

## Original concept

The initial interactive milling concept was provisionally labeled “Elle / Mill” before CoffeeCNC was named and clarified as an independent product. Its original editable source and standalone export are preserved byte-for-byte in [archive](archive/). The current [CoffeeCNC copy](concepts/coffeecnc-mill.html) changes the branding; it does not resolve the workflow shortcomings of the original.

The concept is HTML/CSS/JavaScript with an authored SVG illustration. It is not a photograph, a CAD import, a running LinuxCNC screen or a material-removal simulation. There was no separately generated raster mockup to archive. Its standalone wrapper was exported using the visualization renderer supplied by the authoring environment. External fonts and icons remain prototype dependencies.

## Architecture reference

[Elle](https://github.com/tinic/el-linuxcnc-electron) was inspected at commit `9918ae63a4b038811ca5b42186e7d8ba382f386d`. Its native shell, local web bridge, separation of domain logic from LinuxCNC I/O, and simulation support informed the discussion. No Elle application source has been copied into CoffeeCNC.

## Research

Research documents summarize sources and link to them. They distinguish source observations, proposed product decisions and unverified assumptions. The sample corpus will preserve upstream bytes, commit identifiers, attribution and license files. Presence of a STEP file beside a program is evidence to investigate, not proof of a geometrically verified or machine-ready pairing.
