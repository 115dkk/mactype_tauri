# CupertinoGroup

The Cupertino skin's unit, pinned to Cupertino in this card: a 10px rounded group of 44px rows separated by hairlines inset 16px past the leading slot, with a section title above and a footnote below.

- `CupertinoRow` props: `leading` (a radio, a 20px icon tile with 5px corners, or a status circle), `title`, `description`, `value` (a value, badge, switch or button), `onDisclose` (adds the chevron and makes the whole row a button), `hero` (64px, 15/600 title) and `dirty`.
- Icon tiles use the desaturated `cupertino-tile-*` colours and carry no state; the only saturated colour is the selection and the primary button.
- Disclosure rows open a detail page with a back control; they never expand in place.
- Switches are 26×15 with a white thumb on `cupertino-track`; controls are 26px with 6px corners and a faint shadow.
