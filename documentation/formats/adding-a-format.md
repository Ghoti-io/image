@page image_format_adding Adding a format

# Adding a format

The documentation half of adding a codec. The code half - the stub, the probe,
the load/decode/save callbacks, the allocator and limits contract - is in
`documentation/development.md` under "Adding a new codec"; this page is about
what the format's page must answer before the codec is considered finished.

One format, one page, under `documentation/formats/`. A format does not share
a page with another one: the two grow at different rates, their checklists
answer different questions, and a reader looking for one of them should not
have to scroll past the other. That is how this directory came to exist - PNG
and JPEG shared a single 46 KB file, and BMP, which had been implemented and
tested for some time, was not mentioned in it at all.

## Checklist

- [ ] `documentation/formats/<format>.md` exists, opens with
      `@page format_<format> <Title>`, and is linked from the table in
      \ref image_format_references "Format and specification references".
- [ ] The specification is **named with its version** and linked. A format
      with no formal standard says so and links what documentation there is;
      where that documentation is silent, the page states what the codec does
      and why, because that decision is now the specification as far as this
      library is concerned.
- [ ] Every claim of support says which clause, chunk or marker it refers to.
      "Supports transparency" is not a claim anyone can check; "tRNS (11.3.2.1)
      read for color types 0, 2 and 3" is.
- [ ] Every rejection names the `GIMG_Result` it returns, and why that
      malformation is refused rather than recovered from.
- [ ] Deviations from reference implementations are **listed as cases**, each
      with a fixture behind it. A deviation nobody has written down is a defect
      waiting to be reported as one.
- [ ] Tested scope says where the fixtures come from, how they were generated,
      which external oracle checked which claim, and **where each oracle's
      reach ends**. A round trip through this library alone proves the encoder
      and decoder consistent with each other, not correct; say so where that is
      all there is.
- [ ] The gaps are listed. Absences are cheap to write down while they are
      fresh and expensive to rediscover.
- [ ] `documentation/development.md` and the
      \ref api_options "API options and types" page are updated for any
      option the format adds.
- [ ] The parent `README.md` lists the format among the ones with a codec.

## Page template

```markdown
@page format_<format> <Title>

# <Title>

One paragraph: what the codec implements, and that the claims below are
checked, with a link back to the format index.

## Normative references

- **Specification:** <name, version, link>
- <the individually linked parts, if the document is split>

Which allocator the codec uses for codec-owned allocations.

## Parts implemented

Bullets, one per structural area: containers, color models, bit depths,
compression methods, metadata. Each cites its clause.

## Save

What the encoder chooses and why; which options change it; what it never
writes.

## Compliance checklist

| Area | Supported | Rejected / limitation |
|------|-----------|-----------------------|

## Where this codec differs from <the reference implementations>

| Case | This codec | Elsewhere |
|---|---|---|

## Tested scope

Fixtures and their generator; the oracles and the reach of each; the fuzz
harnesses; the properties asserted without an oracle.

## Not implemented

What is absent, and what closing each gap would take.
```

Open and close the page with a link back to the index, written as
`format_references` in a Doxygen reference command - the first in the opening
paragraph, the last on its own line after a horizontal rule.

## Why the tested-scope section is not optional

The value of these pages is that a claim on them can be traced to something
that runs. The PNG page can say the encoder is about 9% smaller than the
published conformance suite's own files because a sweep measured it; the JPEG
page can say which two samples of 984 the reference codec differs at, and
which way.

The BMP page is the argument for writing the gaps down. It used to say that
its encoder output had **no** external verification - and that sentence, sitting
there in plain sight, is what eventually got an oracle built: `tools/oracle/`
now fetches and builds bmplib, and thirteen encoder outputs are read back by
three decoders that are not ours. The gap closed because it had been named.
The page still names the one that has not closed - `BI_JPEG` and `BI_PNG`
output, which no outside reader will take - so that one has somewhere to start
too.

A format page that only lists features is a marketing document. Name the
oracle, name its limit, and name what nothing checks.
