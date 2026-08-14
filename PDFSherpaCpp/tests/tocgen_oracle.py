"""Print what the Python tocgen.py produces for a PDF, for parity comparison.

Usage:  python tocgen_oracle.py <file.pdf> [...]

Output is one document per block, in a format the C++ parity test also emits:

    ## <basename>
    method=<bookmarks|headings|title>
    <topic>\t<page>
    ...

Run it with a PyMuPDF pinned to the MuPDF release the C++ side links
(pymupdf==1.28.2), or the two are comparing different engines and a mismatch
says nothing about the port.
"""

import os
import sys

# tocgen lives at the repo root, two levels up from PDFSherpaCpp/tests.
sys.path.insert(
    0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

import tocgen  # noqa: E402


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    for path in argv[1:]:
        entries, method = tocgen.generate_entries(path)
        print(f"## {os.path.basename(path)}")
        print(f"method={method}")
        for topic, page in entries:
            print(f"{topic}\t{page}")
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
