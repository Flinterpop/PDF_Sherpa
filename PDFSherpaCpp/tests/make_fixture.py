"""Regenerate the synthetic PDF fixtures used by the C++ test suite.

Run:  python make_fixture.py            (writes beside this script)

The fixtures are committed, so this only needs running when they change.  They
are deliberately synthetic: this repo is public and the corpus PDF Sherpa is
actually used against lives in C:\\ICD and is export-controlled, so no test may
depend on it.

Each fixture drives a different branch of tocgen's three-tier strategy:

  fixture.pdf           has an outline          -> "bookmarks"
  fixture_headings.pdf  no outline, size/bold   -> "headings"
  fixture_title.pdf     one page, nothing       -> "title"

Pin PyMuPDF to the release that wraps the MuPDF the C++ side links, or the
parity comparison compares two different engines:

    pip install pymupdf==1.28.2      # wraps MuPDF 1.28.2
"""

import os

import pymupdf

HERE = os.path.dirname(os.path.abspath(__file__))


def _out(name: str) -> str:
    return os.path.join(HERE, name)


def build_bookmarks_fixture() -> None:
    """Two pages plus an outline: tocgen should take the bookmarks path."""
    doc = pymupdf.open()

    page1 = doc.new_page()
    page1.insert_text((72, 100), "Introduction to the Sherpa",
                      fontsize=18, fontname="hebo")
    page1.insert_text((72, 140),
                      "the quick brown fox jumps over the lazy dog",
                      fontsize=11)
    page1.insert_text((72, 160),
                      "the second body line, also at eleven point",
                      fontsize=11)

    page2 = doc.new_page()
    page2.insert_text((72, 100), "Chapter 2 - the second page", fontsize=14)
    page2.insert_text((72, 140), "more body text on the second page",
                      fontsize=11)

    doc.set_toc([[1, "Introduction", 1], [1, "Chapter 2", 2]])
    doc.save(_out("fixture.pdf"))
    doc.close()


def build_headings_fixture() -> None:
    """No outline, so detection must fall through to heading extraction.

    Deliberately includes the awkward cases the heuristic exists to handle:
      - a bare section number on its own line, to be rejoined with its title;
      - a heading that already carries its own number, which must NOT be
        rejoined with a preceding bare number;
      - noise lines (an email, a date, an author) that must be filtered;
      - a repeated running header, which must be de-duplicated.
    """
    doc = pymupdf.open()

    # Plenty of 11pt body text, so body_size lands on 11 unambiguously.
    body = ("this is ordinary body text set at eleven point and there is "
            "a good deal of it so that it dominates the size histogram")

    page1 = doc.new_page()
    page1.insert_text((72, 60), "Overview of the System",
                      fontsize=16, fontname="hebo")
    page1.insert_text((72, 90), body, fontsize=11)
    page1.insert_text((72, 110), body, fontsize=11)
    # A bare number, then its title on the next line.
    page1.insert_text((72, 150), "5.2", fontsize=14, fontname="hebo")
    page1.insert_text((72, 170), "Message Formats", fontsize=14,
                      fontname="hebo")
    page1.insert_text((72, 200), body, fontsize=11)
    # Noise that _is_noise must drop.
    page1.insert_text((72, 230), "someone@example.com", fontsize=14,
                      fontname="hebo")
    page1.insert_text((72, 250), "3 January 2024", fontsize=14,
                      fontname="hebo")
    page1.insert_text((72, 270), "B Graham", fontsize=14, fontname="hebo")

    page2 = doc.new_page()
    # A heading that already has its own number prefix.
    page2.insert_text((72, 60), "6.1 Timing and Latency", fontsize=14,
                      fontname="hebo")
    page2.insert_text((72, 90), body, fontsize=11)
    page2.insert_text((72, 110), body, fontsize=11)
    page2.insert_text((72, 150), "Appendix A", fontsize=16, fontname="hebo")
    page2.insert_text((72, 180), body, fontsize=11)
    # The same running header as page 1: must appear once, not twice.
    page2.insert_text((72, 210), "Overview of the System",
                      fontsize=16, fontname="hebo")

    doc.save(_out("fixture_headings.pdf"))
    doc.close()


def build_title_fixture() -> None:
    """One page, no outline, too few headings: the title path."""
    doc = pymupdf.open()
    page = doc.new_page()
    page.insert_text((72, 100), "A Solitary Document Title",
                     fontsize=20, fontname="hebo")
    page.insert_text((72, 140),
                     "only one page of body text lives in this document",
                     fontsize=11)
    doc.save(_out("fixture_title.pdf"))
    doc.close()


def main() -> None:
    build_bookmarks_fixture()
    build_headings_fixture()
    build_title_fixture()
    print("pymupdf", pymupdf.VersionBind, "-> mupdf", pymupdf.VersionFitz)
    for name in ("fixture.pdf", "fixture_headings.pdf", "fixture_title.pdf"):
        print("wrote", name, os.path.getsize(_out(name)), "bytes")


if __name__ == "__main__":
    main()
