# Mobile Country Codes (MCC) reference

GeuReflector's `LOCAL_PREFIX` is anchored on a country's **Mobile Country Code**
(MCC) — the 3-digit identifier assigned by the ITU under recommendation E.212
and reused by 3GPP. Anchoring on the MCC keeps each deployment's TG number
space inside its own country namespace, so two countries that both want a
local TG ending in `…01` don't collide. This system is independent of any
mobile or DMR network — the MCC is reused here purely as a stable,
internationally-coordinated numeric prefix.

Use your country's MCC as the leading digits of `LOCAL_PREFIX`, and use the
same prefix for the matching `REMOTE_PREFIX` on every other reflector that
trunks to you. Sub-national TGs extend the MCC (e.g. Lazio = `22201` under
Italy's `222`); longest-prefix-match routes them automatically.

## Common amateur-radio countries

A representative table covering the countries that appear in
[`WW_DEPLOYMENT.md`](WW_DEPLOYMENT.md). For anything not listed, look up the
MCC in your national telecom regulator's allocation or in the ITU-T E.212
list (see "Where to find more" below).

| ISO 3166-1 | Country         | MCC(s)                         | Notes                          |
|------------|-----------------|--------------------------------|--------------------------------|
| AR         | Argentina       | 722                            |                                |
| AT         | Austria         | 232                            |                                |
| AU         | Australia       | 505                            |                                |
| BE         | Belgium         | 206                            |                                |
| BR         | Brazil          | 724                            |                                |
| CA         | Canada          | 302                            |                                |
| CH         | Switzerland     | 228                            |                                |
| CZ         | Czech Republic  | 230                            |                                |
| DE         | Germany         | 262                            |                                |
| DK         | Denmark         | 238                            |                                |
| ES         | Spain           | 214                            |                                |
| FI         | Finland         | 244                            |                                |
| FR         | France          | 208                            |                                |
| IE         | Ireland         | 272                            |                                |
| IT         | Italy           | 222                            |                                |
| JP         | Japan           | 440, 441                       | Two MCC blocks                 |
| MX         | Mexico          | 334                            |                                |
| NL         | Netherlands     | 204                            |                                |
| NO         | Norway          | 242                            |                                |
| NZ         | New Zealand     | 530                            |                                |
| PL         | Poland          | 260                            |                                |
| PT         | Portugal        | 268                            |                                |
| SE         | Sweden          | 240                            |                                |
| UK         | United Kingdom  | 234, 235                       | Two MCC blocks                 |
| US         | United States   | 310, 311, 312, 313, 314, 315, 316 | Seven MCC blocks            |

## Countries with multiple MCCs

A few countries hold more than one MCC allocation. Configure all of them as a
comma-separated list on the country's reflector:

```ini
LOCAL_PREFIX=310,311,312,313,314,315,316   # United States
LOCAL_PREFIX=234,235                       # United Kingdom
LOCAL_PREFIX=440,441                       # Japan
```

Peers trunking to that country should mirror the same list in `REMOTE_PREFIX`.

## Where to find more

The authoritative source is **ITU-T Recommendation E.212**, "List of Mobile
Country or Geographical Area Codes". A more readable cross-reference,
including operator allocations within each MCC, is the public Wikipedia
article "Mobile country code". Both list every country and territory; this
file only mirrors the subset used in the deployment examples in
[`WW_DEPLOYMENT.md`](WW_DEPLOYMENT.md) and [`DEPLOYMENT_ITALY.md`](DEPLOYMENT_ITALY.md).
