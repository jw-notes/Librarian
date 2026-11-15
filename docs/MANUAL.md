# Librarian manual

Everything about Librarian in detail: features and their status, controls, files on the console, servers, and how it is built. The [README](../README.md) is the short version.


A native PS4 homebrew reader for your self-hosted libraries:

- **Knowledge**: Wikipedia and every other ZIM collection on a **Kiwix Server**, with search, links, history, bookmarks and images. The **Kiwix online library** (library.kiwix.org) can be browsed too, also without a server of your own: its collections can be downloaded to a USB drive or the console, or sent to your Kiwix Server through a small download helper (`extras/kiwix-helper`). **ZIM files on a USB drive or the console are read directly**, with no server: articles, pictures, links, title search, random article (no full-text search).
- **Books**: your **Calibre-Web** catalogue over OPDS, with covers, details, search and downloads for offline reading. Signed-in accounts can **upload** a local file to the server, and downloads can be saved to the console or a USB drive.
- **Audiobooks**: your **Audiobookshelf** server: browse and search its libraries, listen with chapters, seeking, speeds from 0.5× to 3× (voices keep their pitch), bookmarks and a sleep timer, with progress and bookmarks kept in sync with the server. Books can be downloaded to the console or a USB drive for listening offline. Your own folders of MP3, M4A/M4B, AAC, FLAC, Opus or Ogg Vorbis files play too, when they are inside the `Librarian` folder of a USB drive. Books you have started appear on Home under **Continue Listening**. The section appears once a server is set up or an audiobook is found.
- **More catalogues**: besides Calibre-Web, any OPDS catalogue can be added (another Calibre, a public library such as Project Gutenberg), with no server of your own needed. Add one from Home's welcome screen, the Books section (OPTIONS) or Settings › Books. The catalogue's name at the top of the Books section's list picks which one is shown (L2 / R2 also switch); the last one used is remembered.
- **Local**: EPUB, MOBI, AZW3, FB2, PDF, CBZ, CBR, TXT, Markdown and HTML files from USB drives, `/data/books` and Librarian's own downloads. Plug in a drive and its documents appear by themselves, with titles, authors and covers; they are read straight from the drive.

Built with the OpenOrbis toolchain. Everything is drawn by Librarian itself at 1920×1080, with no web view and no external services.

**Librarian by [jw-notes](https://github.com/jw-notes)** — Copyright (C) 2026 jw-notes. Free software under the GNU GPL, version 3 or later, with attribution terms under section 7 of the GPL ([NOTICE](../NOTICE)); see [Licence](#licence).

## Features

Librarian 01.00 has been tested on a PS4 (firmware 14.00 with GoldHEN, USB drives read through GoldHEN's FTP server).

| Area | What works |
|---|---|
| Kiwix Server | Collections, title and full-text search, articles with pictures (WebP / JPEG / PNG), tables, links, history, contents, bookmarks |
| Kiwix online library | Browsing by language, category and name; downloads to USB or the console (resumed after an interruption); sending collections to your server with the download helper |
| ZIM files without a server | Collections on USB or the console: articles, pictures, links, title search, random article, delete (old and new ZIM layouts, zstd or xz) |
| Ebooks | EPUB 2 / 3 (chapters, cover, contents, internal links, pictures), MOBI 6, AZW3 / KF8, FB2 (also zipped) |
| Read-along and talking books | EPUB 3 with narration (media overlays), DAISY 2.02 and DAISY 3 (folder or ZIP, full text or audio with headings): read aloud with the passage highlighted and the pages turning; listed under Audiobooks |
| Documents | PDF (MuPDF: zoom, fit, rotation, continuous scrolling, links, contents, bookmarks), Word (DOCX), OpenDocument (ODT), RTF, TXT, Markdown, HTML |
| Tables | CSV and TSV as a grid: fixed header, row card, find, sort |
| Comics | CBZ and CBR (RAR4 and RAR5) |
| Catalogues | Calibre-Web (browse, search, download, sign-in, upload), Project Gutenberg and other OPDS 1 catalogues |
| Audiobooks | Audiobookshelf (sign-in, libraries, search, streaming, chapters, 0.5–3× speed, bookmarks, sleep timer, progress sync, downloads, offline listening), LibriVox, your own folders (MP3, M4A / M4B, AAC, FLAC, Opus, Ogg Vorbis) |
| Local library | USB drives and the console, sorted onto shelves, with covers (the book's own, a picture beside it, or one from Open Library); progress follows a book from slot to slot |
| Interface | 720p, 1080p and 2160p; English, Japanese, Portuguese (Brazil), German, French and Spanish; settings kept encrypted |

### Limits

- DRM-protected and Topaz Kindle books are refused; complex KF8 layouts are reflowed. DjVu is not supported.
- Spreadsheets in Excel (`.xlsx`) or OpenDocument (`.ods`) format are not read: save them as CSV.
- Kiwix collections that are web applications (some interactive ZIMs) cannot be shown.
- Audio in formats other than those above (WMA, Speex, …) plays only through Audiobookshelf's conversion, and cannot be downloaded; DAISY books with WAV audio cannot be played.
- Only OPDS 1 (Atom) catalogues are read.
- External web links are shown but not opened (there is no web browser).
- The translations have not been reviewed by native speakers.

## Install

1. Build the package (below) or take `IV0000-LIBR00001_00-LIBRARIANPS40000.pkg` from the release.
2. Install it with your package installer (GoldHEN: Debug Settings › Game › Package Installer).
3. Open **Librarian**. Home's welcome screen sets things up: ✕ on a step offers what needs no server (the Kiwix online library, Project Gutenberg, LibriVox) or your own server's address. Servers can also be entered in Settings, on the page of their section (**Knowledge**, **Books**, **Audiobooks**; R1 moves between sections), for example `http://192.168.1.10:8080`, with *Connection* to check each.

## Controls

| Where | Button | Action |
|---|---|---|
| Everywhere | L1 / R1 | Previous / next section (Home, Knowledge, Books, Audiobooks, Local, Settings). With nothing set up only Home, Local and Settings are shown; Knowledge appears with a Kiwix Server, the Kiwix online library or ZIM files found, Books with Calibre-Web or a catalogue, Audiobooks with a server or downloaded audiobooks. Home's welcome screen sets each one up (✕ on a step) |
| | D-pad, left stick | Move |
| | ✕ | Open / select |
| | ○ | Back |
| Lists | △ | Options for the item / refresh |
| | □ | Search (Knowledge, Books, Local) |
| Books | OPTIONS | Choose the catalogue, or add one (also: ✕ on the catalogue's name above the sections) |
| | L2 / R2 | Previous / next catalogue |
| Knowledge | OPTIONS | Kiwix online library (also the first card) |
| Online library | △ | Language, category; □ search; OPTIONS downloads |
| Home | △ | On Continue Listening: open the book, or remove it from the row |
| Local | L2 / R2 | Filter: All, Books (ebooks and comics), Audiobooks (downloaded ones and your own folders, ✕ opens the book page; then talking books, ✕ opens them reading aloud), Docs (PDF, DOCX, ODT, RTF, TXT, Markdown, HTML, CSV, TSV), Collections (ZIM files; ✕ opens the collection), Downloaded, USB |
| | OPTIONS | Sort (recently read, title, author) and Refresh |
| | ○ | Clear the search |
| Reader | ← / → | Previous / next page (or screen, when scrolling) |
| | ↑ / ↓, left stick | Scroll, or turn pages in page mode |
| | Right stick | Move the cursor; hover highlights links; Cross opens the pointed link |
| | L2 / R2 | Alternative: select the previous / next link on screen; Cross follows it |
| | □ | Add a bookmark |
| | △ | Contents, bookmarks, favourite, search / random article (Kiwix) |
| | OPTIONS | Text size, typeface, spacing, margins, justification, theme, pages or scrolling |
| | L1 / R1 | Previous / next chapter (EPUB, Kindle, FB2 and text books); back (articles) |
| | ○ | Back to the previous article, or close |
| PDF | D-pad Left / Right | Previous / next page |
| | Left stick | Scroll and pan |
| | Right stick; R3 | Move cursor; R3 toggles cursor / pan mode |
| | L2 / R2 | Zoom out / in |
| | Square | Bookmark the current page |
| | Triangle | Page navigation, jump, contents and bookmarks |
| | OPTIONS | PDF reading tools: fit, actual size, zoom, rotation, continuous scrolling, background, reset |
| | Circle | Close menu, return from an internal link, or close PDF |
| Read-along | △ › *Read aloud* | Reads the book aloud from the page shown (EPUB 3 books with narration, DAISY books); the passage being read is highlighted and pages and chapters turn by themselves. ✕ pauses / resumes; △ › *Narration speed*, *Stop reading aloud*. L1 / R1 move the narration with the chapter. A DAISY book without text starts reading at once |
| Tables | D-pad | Move from cell to cell; L1 / R1 a screen up / down; L2 / R2 a screen left / right |
| | ✕ | The whole row as a card (every column with its name); ← / → previous / next row |
| | □ | Find text in the table (△ › *Find next* for the next match) |
| | △ / OPTIONS | Find, go to a row, first / last row, sort by the selected column (ascending, descending, original order), first row is a header, text size |
| Comics | ← / → | Turn pages; □ whole page or fit width; L2 / R2 zoom; right stick pans; L1 / R1 jump 10 pages |
| Audiobooks | □ | Search |
| | △ | Sort, refresh |
| Player | ✕ | Play / pause |
| | ← / → | Back 15 seconds / forward 30 seconds |
| | L1 / R1 | Previous / next chapter |
| | L2 / R2 | Slower / faster |
| | OPTIONS | Stop (where you stopped is saved); also in △ and in the Audiobooks section's △ menu |
| | □ | Add a bookmark |
| | △ | Chapters, bookmarks, speed, sleep timer, stop |
| | ○ | Back (playing continues) |
| Anywhere, while listening | TOUCH PAD | Open the player |
| Keyboard | D-pad, ✕ | Move between keys, type the key |
| | □ | Delete |
| | △ | Caps (on until pressed again) |
| | R2 | Letters / symbols |
| | L1 / R1 | Move the text cursor |
| | OPTIONS | Done |
| | ○ | Cancel |

## Files on the console

- Settings, library database (positions, bookmarks, history) and log: `/data/Librarian/user-<id>/`
- Books downloaded from Calibre-Web: `/data/Librarian/user-<id>/books/`
- Your own documents: `/data/books/` and USB drives (`/mnt/usb0` … `/mnt/usb7`), searched twelve folders deep. Hidden folders, `System Volume Information` and `$RECYCLE.BIN` are skipped.
- What was found on them: `local_index.json` in the user folder (so the shelf appears at once, and unchanged files are not opened again).
- Covers of your documents: `covers/` in the user folder, small JPEGs named after each file's name and size, so the shelf shows them without opening the books again (from a USB drive read through GoldHEN's FTP server that would mean pulling the files through the server). The folder is emptied and fills again if it ever holds more than 3000.
- Collections downloaded from the Kiwix online library: `zim/` in the user folder, or `Librarian/Kiwix/` on a USB drive. They appear in the Knowledge section.
- If Librarian crashes: `crash.log` in the user folder (the signal, the addresses and the last lines of the log). The next start notes it in `librarian.log` and renames it `crash.prev.log`. Please include it when reporting a problem.

### USB drives

- The PS4 reads USB drives formatted **FAT32 or exFAT**. NTFS is not supported by the console, and a drive formatted as PS4 extended storage is encrypted and cannot be read.
- USB drives live outside the PlayStation application sandbox, so **GoldHEN 2.3 or later** is required. At startup Librarian asks GoldHEN to lift the sandbox for its own process (GoldHEN's jailbreak command, from the GoldHEN Plugins SDK), immediately drops back from root to this user's own uid and groups so files keep this user's ownership, and hands the sandbox back on exit. Without GoldHEN (or with an older one) the escape fails cleanly and only internal storage and downloads are shown. Each step is written to the log (`usb: …`).
- **When GoldHEN cannot lift the sandbox** (its SDK call does not answer on some consoles; seen on firmware 14.00), Librarian reads and writes USB drives through **GoldHEN's own FTP server** on the console (`127.0.0.1:2121`). **FTP must be on in GoldHEN's settings.** Everything works the same — browsing, covers, reading, downloads to USB, audiobooks — but scanning and opening are slower, since every file goes through the server. Settings › System › *USB drives* shows which way drives are reached (*Direct*, *Through GoldHEN FTP*, or *GoldHEN FTP is off*). GoldHEN's server reports no free space, so a full drive only shows when a download fails.
- All eight `/mnt/usbN` folders always exist; a slot counts as connected when its folder is not empty. Librarian checks every 3 seconds while running, so drives can be plugged in or out at any time; Options › Refresh checks at once.
- Books are recognised by file name and size, so progress, bookmarks and favourites carry over when a drive comes back in another slot or a file is moved to another folder. Books on a disconnected drive that you have read stay on the shelf as *Not connected*; the rest disappear until the drive returns. Reading progress is never deleted when a drive is removed.
- The same file in several places (for example a downloaded copy and one on USB) is shown once.
- Books downloaded from Calibre-Web go to the console by default, or to a connected USB drive's `Librarian` folder — set it in Settings › System › *Download books to*.

### Kiwix online library

- Browse it from the Knowledge section's first card (or OPTIONS there); without a Kiwix Server, choose it on Home's welcome screen or in Settings › Knowledge › *Kiwix online library*, and it becomes the Knowledge section. △ picks a language and category, □ searches.
- A collection's page offers **Download to the USB drive**, **Download to this PS4** and **Send to my Kiwix Server**. Downloads run one at a time while Librarian is open; OPTIONS shows them, with progress, and lets you cancel. A download that stops (Librarian closed, network lost) continues where it was when started again.
- Collections can be large (Wikipedia with pictures is over 100 GB). A FAT32 USB drive cannot hold a file over 4 GB: use exFAT. The console's free space cannot be read, so check it before downloading to the PS4.
- Downloaded collections appear in the Knowledge section by themselves (see below).
- **Send to my Kiwix Server** needs the **download helper** (`extras/kiwix-helper/librarian-kiwix-helper.py`, Python 3, see its README) running on the server's computer: it downloads the collection straight into the server's folder, and can add it to kiwix-serve's library or restart a Docker container afterwards. Set its address and key in Settings › Knowledge.

### ZIM files without a server

- ZIM files (Kiwix collections) are found anywhere the Local section looks (every folder of a USB drive, `/data/books`), and in `zim/` in the user folder, where downloads go. They are listed in the Local section (filter *Collections (ZIM)*) and in Knowledge; opening one from Local opens its collection page. They show in the Knowledge section beside your server's collections, marked *This PS4* or *USB drive*; plugging a drive in or out updates the list.
- They read like a server's: main page, links, pictures, history, bookmarks, random article and search by **title** (case does not matter). Full-text search needs a Kiwix Server (the search index inside the file is not read). Collections that are web apps (freeCodeCamp and other interactive ones) cannot be shown, as with a server.
- Files using the old (ZIM 5) and new (ZIM 6) layouts are read, compressed with zstd or xz. Very old files compressed with zlib or bzip2 are not.
- A collection's page has **Delete this file** for files on the console or a drive.
- On a drive read through GoldHEN's FTP server, every page needs a few reads through the server: expect each article to take a moment longer to open.

### LibriVox

- Free public-domain audiobooks, read by volunteers ([librivox.org](https://librivox.org)). Turn them on from Home's welcome screen, the empty Audiobooks section (□), or Settings › Audiobooks › *Free audiobooks*. No account is needed, and no Audiobookshelf server.
- The Audiobooks section then has **LibriVox: new** (books added in the last few months) and, after □, a search by title or author's last name.
- A book's page lists its chapters; **Play** streams them from archive.org, **Download** saves them (MP3) with the cover to the console or a USB drive (Settings › System), for listening offline. Bookmarks, speed, the sleep timer and Continue Listening work as for any audiobook; where you stopped is kept on the console only.

### Languages

- Settings › Appearance › **Language**: Auto (the PS4's system language), English, 日本語, Português (Brasil), Deutsch, Français or Español. Japanese uses Noto Sans JP, shipped in the package.
- Menus, buttons, messages and Settings are translated; some longer explanations are still in English. Book titles, articles and anything a server sends are shown as they are.

### Resolution

- Librarian draws at the resolution the PS4 sends to the TV: 1280×720, 1920×1080, or 3840×2160 (4K, PS4 Pro). Every screen is laid out once and drawn natively at that size: text is rasterised at the real size, and pictures, comic pages and PDF pages are rendered for it, so 4K is sharp and 720p is not a shrunken 1080p.
- Settings › Appearance › **Resolution**: Auto, 720p, 1080p or 2160p, from the next start. 720p draws fastest; 4K draws four times the pixels of 1080p and uses about 100 MB more memory, so on a PS4 Pro 1080p can feel quicker. If the console refuses a resolution (2160p on a PS4 that is not a Pro), Librarian uses 1080p.

### Software update and debug logging

- Settings › System › **Software update** looks for a newer Librarian among the releases of github.com/jw-notes/Librarian (also 30 seconds after each start; a notice appears once if there is one). Selecting it downloads the release's package to `/data/pkg/Librarian.pkg`, checks its size and the SHA-256 GitHub publishes, and then it is installed from Debug Settings › Game › Package Installer. Librarian never installs anything itself; settings and the library are kept.
- Settings › System › **Debug logging** (off by default): every network request and similar detail goes to `librarian.log`. Off, the log keeps start-up, problems (including every failed request) and what Librarian decided. Passwords and keys are never logged.

### Your settings and passwords

- `settings.json` (server addresses, user names, passwords, API and helper keys) is **encrypted** (XChaCha20-Poly1305): its key comes from this console's ID and a random value kept beside it (`.key`). A copy taken off the console, over FTP or in a backup, cannot be read or changed. An older, plain file is encrypted the first time Librarian starts. A file that cannot be decrypted (from another console, or damaged) is set aside as `settings.json.unreadable` and Librarian starts with default settings.
- This protects the file, not the running console: code running on the PS4 itself can derive the same key. The log never contains passwords.

### Covers

- A book's own cover is used: an EPUB's cover image (or, in a picture book that names none, the first picture of its first page), a MOBI / AZW3 / FB2 cover, a comic's or PDF's first page.
- A book without one gets a picture lying beside it with the same name (`Emma.epub` and `Emma.jpg`); a DAISY book also its folder's `cover.jpg`, `folder.jpg` or `front.jpg`.
- Otherwise, with Settings › System › *Find missing covers* on (the default), the cover is looked up on **Open Library** (openlibrary.org) by the title and author the book names itself (books known only by their file name are not looked up). The same goes for LibriVox books without a cover and audiobook folders without a picture ("Author - Title" folders). Covers found, and books with none, are remembered on the console, so each is asked about once. Turned off, nothing is sent.

### Your own audiobook folders

- A folder of audio files (MP3, M4A, M4B, AAC, FLAC, Opus, Ogg or OGA) inside the **`Librarian` folder of a USB drive** (up to three levels down, for example `Librarian/Audiobooks/Jane Austen - Emma/`), or in the console's `audiobooks` folder, is an audiobook. Folders elsewhere on the drive are not looked at.
- The folder's name gives the author and title (`Author - Title`, or just the title); the files play in name order ("Part 2" before "Part 10"), each one a chapter. A picture in the folder (`cover.jpg`, `folder.jpg`, `front.jpg`, or any other) is its cover.
- The first time a folder is found, each file is opened once to measure it, and a `librarian-audiobook.json` is written beside the files, so it is not measured again. Formats the player cannot read (WMA, Speex, …) are left out.
- Where you stopped is kept on the console only: these books are not on your Audiobookshelf server and are never sent to it. Librarian never deletes your files: such a book has no *Delete download* button.

### Read-along and DAISY talking books

- **Where they are listed**: talking books found on the console and USB drives (EPUBs with narration, DAISY books) are under **Audiobooks › Talking books** and in Local's **Audiobooks** filter, as well as on the Books shelf. Opened from Audiobooks, they start reading aloud at once; opened elsewhere, the reader shows *△ Read aloud* at the bottom.
- **EPUB 3 with narration** ("media overlays", as in read-along children's books and many accessible EPUBs): △ › *Read aloud*. The narration starts at the page you are on; the sentence (or passage) being read is highlighted and the pages turn with it. Audio stored uncompressed inside the book is played from it directly; compressed audio is first copied to the console (`narration/` in Librarian's data folder).
- **DAISY talking books**: DAISY 2.02 (a folder with `ncc.html`) and DAISY 3 (a folder with a `.opf` package, an `.ncx` and the DTBook text), or either one in a `.zip` file. They appear on the Local shelf by their title. Books with full text open in the reader, split into sections, with the book's navigation as *Contents*; books with audio and headings only show their headings and start reading aloud at once, the heading being read highlighted. The text files inside a DAISY folder are not listed separately, and such a folder is not taken for an audiobook folder.
- Audio formats are those of the player (MP3, AAC / M4A, FLAC, Opus, Ogg Vorbis). DAISY books using WAV or MP2 audio cannot be played.

### Tables (CSV, TSV)

- `.csv`, `.tsv` and `.tab` files open as a grid, on the Local **Docs** shelf. The separator (comma, semicolon, tab or `|`) is worked out from the first lines; text may be UTF-8, UTF-16 (Excel's *Unicode text*) or Windows-1252.
- The first row is taken as column names when it looks like it (all text, above numbers or distinct values); △ › *First row is a header* changes that. Columns that hold numbers are right-aligned and sort as numbers.
- The selected cell is shown in full under the grid; ✕ shows the whole row, one column per line. Where you were (row and column) is remembered, and the table appears under Continue Reading.
- Limits: files up to 24 MB; past 200,000 rows the rest is left out (a message says so). Excel (`.xlsx`) and OpenDocument (`.ods`) spreadsheets are not read yet: save them as CSV.

### Calibre-Web accounts and uploading

- **Browsing** the catalogue uses OPDS, which authenticates with **HTTP Basic**. If your server has anonymous browsing enabled you need no account; otherwise set Settings › Books › Sign-in to *With an account* and enter your user name and password.
- **Signing in and uploading** use a separate mechanism (a session cookie plus a CSRF token, the same as the web page). From the Local tab, a document's Options menu has *Upload to Calibre-Web*; the account must have the Upload right. Settings › Books › *Test sign-in* checks the account and reports clearly.
- Calibre-Web **limits sign-in attempts** (a few per minute). Librarian signs in only when you ask it to and keeps the session, so it does not trip the limiter; if you see "Too many sign-in attempts", wait a minute.

The Calibre-Web password is stored in `settings.json` in that private folder and is sent only to the Calibre-Web server. The log never contains it.

### Audiobookshelf

- Settings › Audiobooks › Audiobookshelf: the address (usually port 13378), and either your user name and password or an API key (Audiobookshelf 2.26 or newer, made in its Settings › API Keys). The password is sent only when signing in; Librarian then uses the tokens the server gives it and renews them when they expire. A refused sign-in is not retried in the background for a minute, because Audiobookshelf limits sign-in attempts.
- **Formats**: MP3, M4B/M4A/MP4 (AAC, including HE-AAC), FLAC, Opus and Ogg Vorbis are decoded on the console, so they play directly and can be downloaded. For anything else (WMA, Speex, …) Audiobookshelf transcodes to AAC while it plays; such books can be streamed but not downloaded.
- **Progress** is saved on the console every few seconds and on pause, and sent to the server through the play session every 15 seconds. Without the network it waits, together with bookmark changes, and is sent when the server is back; the newer position wins. Audiobookshelf marks a book finished by itself in its last ten seconds; Librarian never undoes that.
- **Downloads** go where Settings › System › *Download books to* says: `/data/Librarian/user-<id>/audiobooks/` or a USB drive's `Librarian/Audiobooks/`. Each book is a folder with its audio files, `cover.jpg` and `librarian-audiobook.json` (chapters and metadata), so it plays with no server at all.
- **Playing in the background**: playback continues while you use the rest of Librarian (a strip in the footer shows it; the TOUCH PAD opens the player).
- **Memory**: about 8 seconds of decoded sound are buffered (1.5 MB); streamed files are read in 256 KB blocks, a dozen kept (3 MB); a long M4B's index takes about 2 bytes per AAC frame (6 MB for 20 hours).

## Building

Requirements: Windows with WSL (the build uses `kali-linux`; any Debian-like distribution works) with `clang-18`, `make`, `python3`, and .NET on Windows for PkgTool.

```powershell
python tools\fetch_deps.py        # stb, miniz, libwebp, GoldHEN SDK, Noto fonts… (pinned, hashes in third_party/PROVENANCE.json)
python tools\make_art.py          # icon0.png and pic1.png (original artwork)
.\tools\build.ps1                 # PS4: compile, sign, package, validate -> dist\dev\
```

The OpenOrbis toolchain and `lld` are expected in `tools/toolchain/` (`OpenOrbis/PS4Toolchain` and `llvm-local/usr/lib/llvm-18/bin/lld`); they are not committed.

Host build (inside WSL): `make host` builds `build/host/librarian`, a Linux build of the same code driven by a controller script (see `source/platform/platform_host.cpp` for the steps). The unit tests and test-only tools (`tests/`) are kept in the development tree only; they are not uploaded or included in releases.

### PDF

PDF support uses MuPDF (AGPL-3.0), built separately once:

```bash
python3 tools/fetch_deps.py --mupdf
tools/build_mupdf.sh host && tools/build_mupdf.sh ps4
```

The Makefile links MuPDF automatically when `build/mupdf-<target>/libmupdf.a` exists; without it, PDFs are reported as unsupported. A build that includes MuPDF (such as the PS4 package) is a combined work distributed under the AGPL-3.0; see [Licence](#licence) for what that requires of anyone who distributes it.

PDF controls are separate from comic controls. OPTIONS offers Fit Page, Fit Width, Actual Size, 25-400% zoom, page navigation, jump by number, contents, 90/180/270 degree rotation, continuous scrolling, four surround colours and Reset View. At 100%, one PDF point is 96/72 screen pixels; this is a logical display scale, not a physical paper measurement on a television. Fit modes may report a different percentage. View preferences and normalized horizontal/vertical resume positions are saved. Circle returns through internal-link history before closing.

PDFs render only visible 512-pixel tiles, with a 48 MB LRU pixel cache and one outstanding metadata/render job per reader. MuPDF retains its separate 64 MB resource store. Large source images still require decoder resources; these limits do not represent a limit on total process memory. External URLs are shown as external links; Librarian has no general web browser.

Text readers share the right-stick cursor, with an 18-unit stick deadzone, a fine-control speed curve, 25-250% adjustable speed in OPTIONS, and a 3.5 second inactivity timeout. Hit-testing uses the current layout and scroll/page offsets, with six pixels of tolerance around small links. L2/R2 remains available for link selection.

### Kindle and FictionBook

MOBI, AZW, PRC and AZW3 open directly. DRM-protected and Topaz Kindle books are refused. Complex KF8 layouts are reflowed rather than reproduced exactly. FB2 may be plain XML, `.fb2.zip` or `.fbz`; its pictures are embedded in the file. DjVu is not supported.


### CBR

RAR archives are read by libarchive's RAR and RAR5 readers, compiled by the Makefile from `third_party/libarchive` with a small configuration (`source/doc/libarchive_config.h`); no other part of libarchive is built.

## How it works

- **Rendering**: a software renderer into a 1920×1080 frame in cached memory, copied to the display on change only. Text uses stb_truetype with glyph and kerning caches.
- **Documents**: Kiwix articles and EPUB, Kindle and FictionBook chapters go through one HTML converter (`source/doc/html_convert.cpp`) into a reflowable document of headings, paragraphs, lists, quotes, code, images, captions and tables. MediaWiki page furniture (edit links, navboxes, collapse controls) is dropped, and infoboxes that the mobile skin hides are shown. A layout engine (`source/doc/layout.cpp`) lays the document out for the chosen text size and column, with justification, table column sizing, page breaks and stable reading positions (block plus offset, so a position survives a text-size change).
- **Background work**: downloads, decoding and layout run on worker threads; results are applied on the UI thread, and are dropped if the screen has closed.
- **Images**: an asynchronous, size-aware cache with a 64 MB budget on the console and two loads at a time. Covers are kept at up to 800 pixels, other pictures 1600; pictures are scaled straight from the decoder's buffer, a few rows at a time. Pictures over 4 megapixels (and PDF pages rendered for covers) are decoded one at a time, and pictures over 16 megapixels are not shown.
- **Audio**: decoders (minimp3, FAAD2 with Librarian's own MP4 and MPEG-TS demuxers, dr_flac) feed a cubic resampler to the console's 48 kHz and a WSOLA time-stretcher, then a ring of about 8 seconds that the output thread hands to `sceAudioOut` in 1024-frame blocks.

## Licence

Copyright (C) 2026 [jw-notes](https://github.com/jw-notes).

Librarian is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version (`GPL-3.0-or-later`; full text in [`LICENSE`](../LICENSE)). It is distributed in the hope that it will be useful, but **without any warranty**; without even the implied warranty of merchantability or fitness for a particular purpose.

[`NOTICE`](../NOTICE) is the authoritative statement of licensing and must accompany every copy. In short:

- **Keep the author's notices and credit.** The GPL requires copies and modified versions to keep the copyright, licence and no-warranty notices intact and to mark changes (sections 4 and 5). In addition, under GPLv3 section 7 (`NOTICE` section 2), any copy, port or modified version must keep these notices, must credit *"Based on Librarian by jw-notes - https://github.com/jw-notes"* in its README and About screen, must keep the credits to every other author, and must not be presented as original work. You may add your own notices for your own changes.
- **MuPDF makes the package AGPL-3.0.** MuPDF 1.24.11 (Artifex Software, AGPL-3.0-or-later) is linked into builds with PDF support, including the PS4 package. Such builds are combined works conveyed under the [GNU AGPL version 3](../licenses/AGPL-3.0.txt) (GPLv3 section 13 allows the combination; Librarian's own code stays GPL-3.0-or-later). Anyone who distributes the package, modified or not, must include `LICENSE`, `NOTICE` and the `licenses/` folder, and must provide the complete Corresponding Source: Librarian at the same version, MuPDF with its bundled libraries, the other components below, and the build and packaging scripts. If you modify Librarian and let people use it over a network, AGPL section 13 also requires offering them your source. A build without MuPDF contains no AGPL code.
- **Releases carry all of this.** The package contains the licences (`/app0/licenses`), and `tools/build.sh` writes next to the `.pkg`: `LICENSE`, `NOTICE`, `licenses/`, the source archives (Librarian, the official unmodified MuPDF 1.24.11 release, the OpenOrbis modules), `SHA256SUMS` and [`SOURCE.md`](../SOURCE.md), which explains how to rebuild the package from them. Publish those files with the package.
- **Third-party code keeps its own licence**, listed below and in detail (versions, copyright holders, required notices) in `NOTICE` section 4. Full texts are in [`licenses/`](../licenses/).

Required notices: *Code from FAAD2 is copyright (c) Nero AG, www.nero.com.* *This software is based in part on the work of the Independent JPEG Group.* *Portions of this software are copyright (C) 2023 The FreeType Project (www.freetype.org). All rights reserved.*

### Third-party components

- [MuPDF](https://mupdf.com) 1.24.11 (AGPL-3.0-or-later) for PDF, with its bundled FreeType (FTL / GPL-2.0-or-later), HarfBuzz (MIT), Gumbo (Apache-2.0), IJG libjpeg (IJG licence), Little CMS (MIT), MuJS (ISC), zlib (zlib), jbig2dec (AGPL-3.0-or-later), OpenJPEG (BSD-2-Clause) and extract (AGPL-3.0-or-later)
- [FAAD2](https://github.com/knik0/faad2) 2.11.4 (GPL-2.0-or-later) for AAC audio, [minimp3](https://github.com/lieff/minimp3) (CC0-1.0) for MP3, [dr_flac](https://github.com/mackron/dr_libs) (public domain or MIT-0) for FLAC, [libopus](https://opus-codec.org) 1.5.2 (BSD-3-Clause) for Opus and [stb_vorbis](https://github.com/nothings/stb) (MIT or public domain) for Vorbis
- [libarchive](https://www.libarchive.org) 3.7.7 RAR / RAR5 readers (BSD-2-Clause; its BLAKE2 files CC0-1.0, OpenSSL or Apache-2.0) for CBR
- [Monocypher](https://monocypher.org) 4.0.2 (CC0-1.0 or BSD-2-Clause), which encrypts settings.json
- [Noto Sans JP](https://github.com/notofonts/noto-cjk) 2.004 (SIL OFL 1.1), for Japanese
- [zstd](https://github.com/facebook/zstd) 1.5.6 decoder (BSD-3-Clause) and [XZ Embedded](https://tukaani.org/xz/embedded.html) 2024-12-30 (0BSD), for ZIM files
- [libwebp](https://github.com/webmproject/libwebp) 1.4.0 decoder (BSD-3-Clause), [miniz](https://github.com/richgel999/miniz) 3.0.2 (MIT), [stb_image, stb_image_write, stb_truetype](https://github.com/nothings/stb) (MIT or public domain)
- [Noto Sans, Noto Serif, Noto Sans Mono](https://github.com/notofonts/latin-greek-cyrillic) (SIL Open Font License 1.1; `OFL.txt` is shipped with the fonts in the package)
- [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) 0.5.2 (GPL-3.0): the package includes its `libc.prx`, `libSceFios2.prx` and `right.sprx` modules, and the program links its musl libc fork (MIT) and LLVM libc++ / compiler-rt (Apache-2.0 WITH LLVM-exception)
- [GoldHEN Plugins SDK](https://github.com/GoldHEN/GoldHEN_Plugins_SDK) (MIT, Copyright (c) 2022 GoldHEN) for the USB sandbox escape: its SDK command wrappers and syscall stub, in `third_party/goldhen_sdk`

### Credits

Developed by [jw-notes](https://github.com/jw-notes).

- MOBI/KF8 format references: [KindleUnpack](https://github.com/kevinhendricks/KindleUnpack) and [Calibre](https://github.com/kovidgoyal/calibre) (GPL-3.0); Kindle and FictionBook use Librarian's shared HTML reader
- [GoldHEN](https://github.com/GoldHEN) by SiSTR0 and the GoldHEN team: the homebrew enabler, its Plugins SDK (MIT) and its FTP server, through which USB drives are reached
