Liberation Sans Regular and Bold are bundled under the SIL Open Font License (see LICENSE).
The original, unmodified TTFs are embedded by cmake/EmbedFont.cmake; the GUI never
looks up a font on the host filesystem. The same bytes are baked at 11–16 CSS pixels
for the UI's text sizes. Missing or invalid font data is a startup/build error.
