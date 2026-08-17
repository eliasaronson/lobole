# Using the Onshape importer: what their terms say

> Read 2026-08-17 from the public pages linked below. **This is a summary of
> what those pages say, not legal advice**, and the one document that governs
> API access most directly is not public. Anyone using
> `tools/onshape_import.py` is responsible for their own compliance.

The importer (`tools/onshape_import.py`, interchange I3) reads *your own*
Onshape documents with *your own* API keys and replays their feature history
into Designer. That shape of use — a user exporting their own content — is
what the Terms of Use contemplate when they say users own their Content and
that Onshape "enables export of Content in Parasolid (.x_t) and/or other
industry standard file formats". Three things around it are worth knowing
before anyone relies on it.

## 1. There is a separate API Agreement, and it is not public

The Product Terms of Use are explicit:

> IF YOU ACCESS THE SERVICE THROUGH ANY MEANS OTHER THAN AN UNMODIFIED
> ONSHAPE MOBILE OR WEB CLIENT, THEN IN ADDITION TO THIS AGREEMENT YOU ALSO
> ARE SUBJECT TO THE TERMS AND CONDITIONS OF ONSHAPE'S API AGREEMENT.

That agreement is presented on first access to the developer portal and is not
published on the legal index, so it is **not summarized here** — read it in
your own account before using this tool. It is the document most likely to
contain the operative restrictions.

## 2. The competitive-use clause is the one to read carefully

The Terms of Use prohibit using the Service

> to build a similar or competitive service offering

and prohibit analysing the Service "for purposes competitive to Onshape".
Designer is a parametric CAD application. A reasonable reading is that pulling
your own models across for your own use is fine and that studying their
service in order to build a rival is not — but the line between those is a
judgement call, and this project sits closer to it than most API consumers do.
That is a risk to take deliberately rather than discover.

## 3. API calls are a metered annual budget, not a rate limit

Per the developer docs, usage is capped **per year**, by account type:

| Account | Annual API calls |
| --- | --- |
| Free / Standard / EDU student | 2,500 per user |
| Professional | 5,000 per user |
| Enterprise (and EDU Enterprise) | 10,000 |

Only successful calls (2xx/3xx) with an API key count; failures do not.
Exceeding a per-endpoint rate returns **429**; exhausting the annual budget
returns **402**. So a full document import is not free in the way a local tool
is — a handful of imports and their retries is a visible fraction of a free
account's yearly allowance. Prefer `dump` once and replay from the saved JSON
(which is what the fixtures under `tests/fixtures/onshape/` are) over
re-fetching while iterating.

## Sources

- Product Terms of Use — <https://www.onshape.com/en/legal/terms-of-use>
- Legal index (Terms, Trademarks, Data Disclosures) —
  <https://www.onshape.com/en/legal/>
- API rate and usage limits —
  <https://onshape-public.github.io/docs/auth/limits/>
- Trademarks: Onshape®, Onshape Full-Cloud CAD®, FeatureScript™ are PTC
  Inc.'s — <https://www.onshape.com/en/legal/trademarks>

Their trademark page lists the marks but gives no third-party usage guidance;
Designer's own notice is in the README under *Trademarks*.
