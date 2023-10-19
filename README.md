# pihpsdr
Standalone code for HPSDR,
supporting both the old (P1) and new (P2) HPSDR protocols, as well as the SoapySDR framework.

It runs on Linux (including RaspPi 3/4) and MacOS (using the "Homebrew" working environment).

**Consult the Manual (Appendices H, I, J) on how-to install and or compile piHPSDR
on your machine (Raspberry PI binary installation, Raspberry PI compile from
sources, and MacOS compile from sources).**

Latest features:

- in-depth (about pages) manual (file release/piHPSDR-Manual.pdf)
- automatic installation procedures, a binary-only installation for RaspPi only,
  and a "compile from sources" procedure for Linux (including RaspPi) and MacOS
  (see Appendices H, I, J of the manual)
- dynamic screen resizing in the "Screen" menu, including transitions
  between full-screen and window mode
- PureSignal now works with Anan-10E/100B in P1
- HermesLite-II drive slider now working smoothly
- CW audio peak filter (in the Filter menu)
- Improved layout for nearly all menus
- Option to make pop-down menus (combo boxes) "TouchScreen-Friendly" (Radio Menu)

Full source code download using git:
git clone https://github.com/dl1ycf/pihpsdr.git

**Installation by compiling from the sources (see the Manual,
Appendix I for Linux and Appendix J for MacOS) is highly recommended,
since the binary-only installation (see the Manual, Appendix H)
is not much easier, and binaries may cease to work across OS upgrades.**

