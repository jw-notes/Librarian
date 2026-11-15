# Librarian's Kiwix download helper

A small program that runs on the computer with your Kiwix Server. When you
pick a collection in Librarian's **Kiwix online library** on the PS4 and
choose **Send to my Kiwix Server**, the helper downloads it from the online
library straight into the server's folder. The PS4 does not have to download
and copy the file, and it can be turned off while the download runs.

It needs Python 3 and nothing else. It only downloads from the Kiwix online
library (kiwix.org), only into the folder you give it, and only for requests
that carry its key.

## Starting it

```
python3 librarian-kiwix-helper.py --folder /path/to/your/zim/files
```

On its first start it makes a short key, keeps it in
`<folder>/.librarian-helper-key`, and prints it. In Librarian, open
**Settings › Knowledge**, and under the Kiwix online library set:

- **Download helper**: the computer's address and the helper's port, for example
  `http://192.168.1.10:8091`;
- **Helper key**: the key it printed.

**Check the helper** then says whether it answers and where it saves.

## Making Kiwix Server see new collections

kiwix-serve does not notice new files on its own. Pick what matches how you
run it:

- **With a library file** (`kiwix-serve --library library.xml --monitorLibrary`):
  start the helper with `--library /path/to/library.xml`. Each finished file is
  added with `kiwix-manage` (from kiwix-tools), and kiwix-serve picks it up.
- **In Docker** (the `kiwix-serve` image serving `*.zim` from its data folder):
  start the helper with `--after "docker restart <container name>"`, so the
  container restarts after each download.
- **Neither**: restart kiwix-serve yourself after a download.

## Options

| Option | Meaning |
|---|---|
| `--folder` | where the ZIM files go (required) |
| `--port` | port to listen on (8091) |
| `--bind` | address to listen on (all) |
| `--key` / `--key-file` | a key of your own, or where it is kept |
| `--library` | kiwix-serve's library.xml, updated with `kiwix-manage` |
| `--kiwix-manage` | where `kiwix-manage` is, if not on the PATH |
| `--after` | a command run after each download |
| `--any-source` | also accept addresses outside kiwix.org |

Downloads that stop part-way leave a `.part` file. Sending the same collection
again continues it. The helper refuses a download that does not fit in the
folder's free space.
