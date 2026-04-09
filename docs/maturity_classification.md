# maturity classification summary

## logic order used in sketch

1. no data / saturated reading
2. spoiled
3. weak reading
4. unripe
5. unripe → ripe transition
6. ripe
7. ripe → overripe transition
8. overripe
9. fallback by strongest visible band

## hard stage thresholds

| stage | main rule used |

| Spoiled | `NIR/Clear > 0.30` and/or very dark reading with late-stage red signs, with `F8/F6 >= 1.00` and `F7/F5 >= 1.30` as strong decay indicators |
| Green / Unripe | `F5/F7 > 1.20` and `F4/F8 > 1.10` |
| Ripe | `Clear >= 1600`, `F7/F5 = 0.80 to 1.15`, and `F8/F6 <= 0.85` |
| Overripe | `Clear >= 900` and either `F7/F5 > 1.28` or `F8/F6 > 0.95` |

## transition bands

| transition zone | rule used |

| Unripe → Ripe | `Clear >= 1200`, `F5/F7 > 1.00 and <= 1.20`, `F4/F8 > 1.00 and <= 1.10` |
| Ripe → Overripe | `Clear >= 1200`, and either `F7/F5 > 1.15 and <= 1.28` or `F8/F6 > 0.85 and <= 0.95` |

## note

the transition zones are added as narrow buffer bands between the original hard stages, so borderline samples are labeled more smoothly instead of flipping directly from unripe to ripe or from ripe to overripe.
