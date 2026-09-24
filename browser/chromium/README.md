# Chromium extension

Load this directory as an unpacked extension in Chrome or Chromium. Copy its ID
from the browser's extension page and register the native host with
`cdm browser install --chrome --id <extension-id>` or `--chromium`.

The extension captures HTTP(S) downloads through `downloads.onCreated`, asks
cdm to review them, and immediately requests browser cancellation. A browser
download can briefly start and leave a partial file. URL-only GET transfer is
supported; authenticated, POST, Blob, and data URL downloads are outside this
first release. A red `!` on the extension button reports native-host or
download errors; inspect the extension service-worker console for details.
