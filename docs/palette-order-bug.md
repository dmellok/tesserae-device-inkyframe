# Server bug: palette-profile slot order permutes Inky 7-colour frames

Found 2026-09-06 while bringing up this firmware against Tesserae v0.392.0.
It is a **server-side** bug. The firmware and the panel are not involved.

## Symptom

Every frame sent to an Inky 7-colour (ACeP) panel has four of its seven colours
swapped in pairs: green appears as yellow, yellow as green, blue as red, red as
blue. Black, white and orange are unaffected. Photographs look wrong in a way
that is hard to name, because skin tones land on the wrong primaries.

## Evidence

A test image of seven labelled bars, one per palette colour, was pushed to a
panel and the packed `.bin` the server produced was read back byte for byte.

| Bar in the source image | Index the server packed | What the panel painted |
|---|---|---|
| black | 0 | black |
| white | 1 | white |
| green | **5** | yellow |
| blue | **4** | red |
| red | **3** | blue |
| yellow | **2** | green |
| orange | 6 | orange |

The panel painted every index exactly as `INKY_7COLOUR_PALETTE` in
`app/quantizer.py` defines it, so the panel and firmware are correct. The
server chose the wrong indices.

## Cause

`app/quantizer.py` defines the gamut's canonical order, which is also the
order the Pimoroni `inky` library and the panel's own palette register use:

```python
INKY_7COLOUR_PALETTE = (black, white, green, blue, red, yellow, orange)
_INKY_7COLOUR_NIBBLE_BY_PALETTE_INDEX = (0, 1, 2, 3, 4, 5, 6)   # identity
```

`app/palette_profiles/schema.py` emits a profile's palette in a different,
fixed order:

```python
def as_tuples(self):
    for hex_value in (self.black, self.white, self.yellow, self.red,
                      self.blue, self.green):
```

That is the Spectra 6 / Waveshare E6 slot order. It is correct for the
`waveshare_e6` family and for `bwry_4` (which uses only the first four slots),
but for `inky_7colour` it swaps slots 2 with 5 and 3 with 4.

When a profile is active, `pack_to_panel_bin` takes it as `palette_override`
and replaces the palette used for nearest-colour matching, **while keeping the
gamut's identity nibble LUT**. So a green source pixel matches override slot 5
and is written as nibble 5, which the panel paints yellow.

The same measured colours exist twice in the codebase, in contradictory orders,
which makes the mistake easy to see:

- `INKY_7COLOUR_CALIBRATED_PALETTE` in `app/quantizer.py`: black, white,
  **green, blue, red, yellow**, orange.
- The `paperlesspaper-inky7` profile in `app/palette_profiles/bundled.py`:
  the same seven hex values, emitted as black, white, **yellow, red, blue,
  green**, orange.

## Blast radius

`default_slug_for("inky_7colour")` returns `paperlesspaper-inky7`, so this is
the default for every Inky ACeP panel, not an opt-in. Any such device with a
palette profile set renders with the swap. Spectra 6 panels are unaffected,
because their canonical order happens to match the slot order.

## A second bug: "no profile" cannot be selected

The obvious workaround, choosing **Built-in default (no profile)** in the
Calibration picker, does not hold. The apply route stores the empty slug
correctly, but the device-card section builder in
`app/settings/index_routes.py` treats an empty slug as stale and writes the
family default back on the next page render:

```python
if slug and _resolves_to_profile(slug):
    return slug
fallback = default_slug_for(family)          # "paperlesspaper-inky7"
if slug != fallback:
    settings_store().update_for_namespace(
        "devices", device.id, {"palette_profile_slug": fallback}, ...)
return fallback
```

`slug` is falsy after a clear, so the guard is skipped and the default is
restored. Verified on a live server: clearing the profile, then reloading
Settings, leaves `palette_profile_slug` back at `paperlesspaper-inky7`.

The self-heal is reasonable for a *stale* slug that no longer resolves. It
should not fire for a deliberately empty one. Suggested fix: only self-heal
when the slug is non-empty and fails to resolve.

Between the two bugs, an Inky 7-colour panel has no working escape through the
UI, which is why the workaround below has to be a profile rather than the
absence of one.

## Workaround

Save a user profile with the affected fields permuted, so the slot order and
the gamut order cancel out:

| Field | Value | Actual colour |
|---|---|---|
| black | `#191E21` | black |
| white | `#F1F1F1` | white |
| yellow | `#53A428` | green |
| red | `#31318F` | blue |
| blue | `#D20E13` | red |
| green | `#F3CF11` | yellow |
| orange | `#B85E1C` | orange |

A ready-made profile is in
[server/palette_profiles/inky7-slot-order-fix.json](../server/palette_profiles/inky7-slot-order-fix.json).
Drop it into `data/palette_profiles/` in the Tesserae install and pick it in the
Calibration tab. It resolves to a real profile, so the self-heal leaves it
alone. Delete it once the slot order is made gamut-aware upstream, because it
will then invert the colours it currently corrects.

## Suggested fix upstream

Make the slot-to-gamut mapping explicit rather than positional. Either give
`as_tuples()` a `gamut` argument and reorder for `inky_7colour`, or have
`resolve_device_palette` return the tuple already permuted into the gamut's
canonical order. A regression test that renders a solid green frame for an
`inky_7colour` device with the default profile and asserts every nibble is `2`
would have caught it.
