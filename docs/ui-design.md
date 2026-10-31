# Dual-screen UI design

The spec for the 3DS UI, from the study of DS/3DS ports and remakes (Dragon Quest VII/VIII 3DS, Bravely Default,
Chrono Trigger DS, Shin Megami Tensei IV, Fire Emblem Awakening, Xenoblade 3D, Ocarina of Time 3D, Pokémon). Each
item is marked **[done]**, **[partial]** (what is missing follows) or **[todo]**.

## Principles
- The top screen is the world; the bottom screen is information and decisions. **No mirroring anywhere.**
- No virtual D-pad or face buttons: the 3DS has them. Touch is an optional shortcut; every touch action is also
  reachable with the buttons, with the PS1's cursor model. Touch targets ≥ 40×32 px, fixed places, a gap between
  them, nothing destructive next to something common.
- Nothing on the bottom screen is needed in the moment of action: speech, "!" prompts, damage numbers, ability names
  and the target cursor stay on top.
- Top-screen 2D UI sits at the screen plane in stereo, the scene behind it; every layout also works in 2D.
- FF9's menus are laid out for ~320×224: on the 320×240 bottom screen they fit nearly 1:1. Move them there, add
  touch zones over their items, and design new top-screen detail panels.

## Field
- **Top**: the field at full width, stereo depth from the background layers; the "!"/"?" balloons and the speech
  bubbles stay here. **[done]**
- **Bottom, when no window is open**:
  - **Area map** from the walkmesh (top-down), Zidane's position and facing **[done]**; the exits (gateways from the
    event scripts) **[todo: decomp request R30]**; save moogles and shops once visited **[todo]**.
  - **Header line**: location name, gil, play time. **[todo: in separate boxes now]**
  - **Compact party strip**: four rows of portrait, HP and MP, status icons. **[partial: names and HP]**
  - **Large edge buttons** (≥ 40×32): **Menu** (also △) **[done]**; **ATE**, pulsing when an Active Time Event is
    available (Select stays Select) **[todo: R30]**; **Cards** with the collection count **[todo]**; **Mognet**
    with unread letters, enabled at a moogle **[todo]**; **Chocograph / hints** when relevant **[todo]**.
  - Nothing here is needed while walking.

## Dialogue
- The text stays on top in FF9's speaker-anchored bubbles, at the screen plane. **[done]**
- The bottom can show a scrollable **dialogue backlog** (new). **[todo]**
- **Choice prompts**: the choices as large touch buttons on the bottom while the question stays on top; cursor and
  A still work (an input aid, not a mirror). **[todo]**

## Battle
- **Top**: the battle scene with no command or status windows **[done]**; damage and heal numbers **[done]**; the
  ability-name banner **[done]**; the PS1 target hand in the scene **[done]**; a "ready" marker on the character
  whose turn it is **[done: the game's own]**; optionally a thin row of ATB pips along the top edge in Active mode
  **[todo]**.
- **Bottom**:
  - **Left ~45%: command list**, large rows (Attack, Steal/Skill, Defend, Item, Change/Run); sub-lists (abilities,
    items) replace it in place with MP cost and count, and a description line at the bottom (the PS1 help text).
    **[todo: the PS1 windows at 1:1 for now]**
  - **Right: four party cards**: name, HP, MP, ATB bar and Trance bar, status icons; tapping a ready character's
    card switches to them (PS1 △/✕). **[todo]**
- **Targeting**: the command area becomes an **enemy/ally panel laid out like the formation**, a large name button
  per enemy (HP once Libra was used); tap selects, tap again confirms; a **single / all** toggle for R; the top
  cursor follows; physical controls drive the same state. **[partial: the game's ENEMY/PLAYER target window, one
  tap selects and confirms]**
- Wait mode: opening a sub-list pauses ATB exactly as on PS1; no timing changes. **[done: game logic unchanged]**

## Main menu
- **Bottom**: the PS1 menu with touch zones. **[partial: 1:1, taps steer its hand cursor]**
- **Top**, per sub-menu: Equip: a rotating 3D model in the selected gear, before→after stats; Abilities: AP progress
  for every ability the gear teaches; Status: full stat sheet and a large portrait; Items: description and who can
  use it. Game logic pauses; never a dimmed field. **[todo: the frozen field for now]**

## Shops and synthesis
- **Bottom**: item list, a quantity stepper (large +/-, L/R ×10), Buy/Sell tabs. **[partial: the PS1 shop 1:1]**
- **Top**: the "who can equip ↑/↓" party panel large, the stat change for the highlighted character, the abilities
  the item teaches; synthesis adds ingredients and how many you own. **[todo]**

## Save screens (moogle)
- **Bottom**: large slot buttons. **[partial: the PS1 save screen 1:1]**
- **Top**: the selected slot's details (party, location, time, gil). **[todo]**
- Overwriting needs a separate confirmation, never one tap. **[done: the game's own confirm]**

## World map
- **Top**: the 3D world without the minimap. **[done]**
- **Bottom**: a large world map with the player or vehicle arrow and discovered places **[done]**; tapping a place
  shows its name **[todo]**; a zoom toggle **[todo]**; in chocobo mode the Chocograph and dig hints **[todo]**.
- Vehicle controls stay physical. **[done]**

## Tetra Master
- **Bottom**: the 4×4 board with both hands (cards ~42×51: four columns ~176 px, the two 5-card hands as
  overlapping columns either side); tap or drag to place. **[partial: the PS1 frame 1:1, taps steer the cursor]**
- **Top**: the highlighted card large and readable (arrows, the "1P2A" string), the opponent's portrait and the
  score, battle and combo flips in stereo depth. **[partial: a 2x magnifier following the cursor]**
- D-pad and A still place cards. **[done]**

## Mognet
- **Bottom**: letters and moogles, a Send button. **[todo]**  **Top**: the open letter. **[todo]**

## FMVs
- **Top**: the video at the screen plane, realtime overlays with it. **[done]**
- **Bottom**: black or a static location card. **[done: black]**
