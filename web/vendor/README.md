Vendored xterm.js.  These three files are the terminal the page runs in; they
used to be three CDN tags and are now shipped with it.

Why they are here at all: release.yml stages romwbw.html, romwbw.js,
romwbw.wasm and roms/ into the deb and the rpm.  Nothing staged xterm, so an
installed package on a machine with no internet opened a page with no terminal
in it - the emulator ran and nothing could be seen or typed.  Serving them from
this directory also removes the last third-party host the page talks to.

What is here, and where it came from:

	vendor/xterm.css	xterm 5.3.0, package/css/xterm.css
	vendor/xterm.js	xterm 5.3.0, package/lib/xterm.js
	vendor/xterm-addon-fit.js	xterm-addon-fit 0.8.0, package/lib/xterm-addon-fit.js
	vendor/LICENSE.xterm	xterm 5.3.0, package/LICENSE (MIT)
	vendor/LICENSE.xterm-addon-fit	xterm-addon-fit 0.8.0, package/LICENSE (MIT)

Taken from the npm registry tarballs, not from a CDN:

	https://registry.npmjs.org/xterm/-/xterm-5.3.0.tgz
	https://registry.npmjs.org/xterm-addon-fit/-/xterm-addon-fit-0.8.0.tgz

The two LICENSE files are both MIT and differ only in the copyright years, so
they are kept separately rather than merged.

SRI: there is no integrity attribute on the tags any more, and that is not a
step backwards.  Subresource integrity is a check on a file fetched from a
host you do not control; these are same-origin files shipped inside the
package, so the file IS what a hash would have been taken over.  For the
record, and so a future update can be checked against what shipped, the three
sha384 values that were in those tags are exactly the digests of the files in
this directory:

	xterm.css	sha384-LJcOxlx9IMbNXDqJ2axpfEQKkAYbFjJfhXexLfiRJhjDU81mzgkiQq8rkV0j6dVh
	xterm.js	sha384-/nfmYPUzWMS6v2atn8hbljz7NE0EI1iGx34lJaNzyVjWGDzMv+ciUZUeJpKA3Glc
	xterm-addon-fit.js	sha384-AQLWHRKAgdTxkolJcLOELg4E9rE89CPE2xMy3tIRFn08NcGKPTsELdvKomqji+DL

Regenerate with:

	openssl dgst -sha384 -binary FILE | openssl base64 -A

Do not reach for the .min.js names when updating.  xterm@5.3.0 ships
lib/xterm.js already minified and no .min.js at all; jsdelivr synthesises that
name and serves the same bytes with its own banner prepended, and the banner
itself says "Do NOT use SRI with dynamically generated files".  The registry
paths above are the real ones.
