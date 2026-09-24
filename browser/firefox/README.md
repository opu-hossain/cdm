# Firefox extension

Load this directory as a temporary extension for development. Its stable Gecko
ID is `browser@cdm.local`; register the native host with
`cdm browser install --firefox --id browser@cdm.local`.

The extension captures HTTP(S) downloads through `downloads.onCreated`, asks
cdm to review them, and immediately requests browser cancellation. A browser
download can briefly start and leave a partial file. URL-only GET transfer is
supported; authenticated, POST, Blob, and data URL downloads are outside this
first release. A red `!` on the extension button reports native-host or
download errors; inspect the extension background console for details.
