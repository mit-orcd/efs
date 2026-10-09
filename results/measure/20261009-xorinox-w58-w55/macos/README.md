# macOS NFS follow-up

Plain cp fails copying source com.apple.provenance, including an unsandboxed
repeat (prepare-native.log). cp -X passes 20 warm copies (ledger-data.json).
Fresh gateway verification passes 20 byte hashes (gateway-cold-bytes.log).
After the user unmounted/remounted macOS NFS, all 20 exact hashes, lengths
and inode identities passed (verify-macos-cold.log and ledger-data.json).
These are client/gateway cold-state results, not storage-crash results.
The two failed plain-copy directories are retained, as is the data-only copy
directory as retained test evidence. Gateway remount completed before the
trailing local SSH-config refresh permission error.
