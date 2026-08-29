# Address families

Observed by comparing the 22 converted profiles. It is not from documentation,
because B/S/H/ publishes none, so treat it as a strong pattern rather than a
specification.

## Addressing follows the generation, not the appliance

The 22 profiles fall into two clear address families plus a long tail:

| Family | Addresses | Devices |
| :--- | :--- | :--- |
| **A** | `0x11`, `0x21` | 3 dryers, 4 washing machines |
| **B** | `0x14`, `0x15`, `0x26` | 5 washing machines |
| tail | various | dishwashers, oven, fridge, steamer, 1 dryer, 2 washing machines |

Family A contains **both dryers and washing machines**. Whatever determines the
addressing, it is not the kind of appliance.

## What is genuinely identical

Within family A, the generic machine state sits at the same address with the same
encoding, across appliance types:

| Address | Meaning | Encoding | Seen on |
| :--- | :--- | :--- | :--- |
| `0x21.1002` | remaining time | `u16be` at 0 | 3 dryers, 3 washing machines |
| `0x21.1000` byte 0 | machine state | `u8` | 3 dryers, 3 washing machines |
| `0x21.1000` byte 1 | door | `u8` | 1 dryer, 1 washing machine |
| `0x21.1004` | finished | `u8` at 0 | 2 dryers |
| `0x11.1001` | ready | `u8` at 0 | 2 dryers |

Among the three family-A dryers, **every** shared address decodes identically --
same operation, same byte offset, no exceptions:

```
0x11.1006  program           u8   @0   WT47R440, WT47W5W0, WTW85460DE
0x11.1006  anti_crease_time  i8   @1   WT47R440, WT47W5W0, WTW85460DE
0x11.1006  fine_adjust       u8   @4   WT47R440, WT47W5W0, WTW85460DE
0x11.1006  low_heat          mask @6   WT47R440, WT47W5W0, WTW85460DE
0x21.1000  door              u8   @1   WT47R440, WT47W5W0, WTW85460DE
0x21.1002  time_remaining    u16be@0   WT47R440, WT47W5W0
```

Three models from two brands, one shared decoding. The value *tables* still
differ (programme names are per model) but the extraction does not.

## Where it breaks, and why that is dangerous

`0x11.1006` is the appliance-specific settings block. Same address, entirely
different contents:

```
dryers            program@0   anti_crease@1  fine_adjust@4  low_heat@6
washing machines  rpm@1       temperature@2  features@5
```

A washing-machine profile applied to a dryer will read `0x11.1006` byte 2 and
report a temperature. It will produce a number in the right range. It will not
be a temperature.

This is the concrete reason the firmware ranks profiles against observed traffic
instead of asking the user for a model number, keeps recording addresses no
profile explains, and puts the profile override next to the suggestion rather
than behind it. A wrong profile does not fail; it lies quietly.

## What is family-wide, checked rather than assumed

Eight definitions appear on both dryers and washing machines within family A.
Six carry the same meaning; two do not:

| Address | Dryer | Washing machine | Same? |
| :--- | :--- | :--- | :--- |
| `0x21.1002` | remaining time | remaining time | yes |
| `0x21.1000` byte 0 | state | state | yes |
| `0x21.1000` byte 1 | door | door | yes |
| `0x11.1001` byte 0 | ready | ready | yes |
| `0x11.1006` byte 0 | which programme | which programme | yes |
| `0x11.1006` byte 2 | **drying target** | **temperature** | **no** |
| `0x11.1006` byte 4 | **drying degree** | **spin speed** | **no** |

The last two are indistinguishable from a structural comparison: same address,
same extraction, same width. Only the names reveal that they are different
quantities. So the family base is a reviewed list, not a computed one --
anything hoisted across appliance types had to be checked by reading, and
`0x11.1006` bytes 2 and 4 are deliberately left with the appliance-type bases.

This is why the profile hierarchy has three levels:

```
base/family-a          ready, programme, state, door, remaining time
   +- base/dryer       drying target, drying degree, anti-crease, low heat
   +- base/washing-machine  temperature, spin speed, features
        +- WM14S750    names and programme tables
```

## Practical consequence

For an unknown family-A appliance, the generic values (door, remaining time,
state) will very likely be right under any family-A profile. The programme and
settings block will not, unless the profile is for the same appliance type.

So a partially-correct reading is the expected outcome of a near miss, not a
sign that the profile is right.
