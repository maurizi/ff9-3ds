# Dual-screen UI design

The spec for the 3DS UI, from the study of DS/3DS ports and remakes (Dragon Quest VII/VIII 3DS, Bravely Default,
Chrono Trigger DS, Shin Megami Tensei IV, Fire Emblem Awakening, Xenoblade 3D, Ocarina of Time 3D, Pokémon). Each
item is marked **[done]**, **[partial]** (what is missing follows) or **[todo]**.

## Button icons
- The PS1's button icons, wherever the game draws them (dialogue, help, menus), are the 3DS's: A for ✕, B for ○,
  X for △, Y for □, L/R, ZL/ZR, from the system font's own glyphs (U+E000-E005). **[done]**

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
    event scripts) **[done: the floor in or by a region whose player-inside function changes field, shaded green]**;
    save moogles and shops **[done: moogles; shopkeepers that open their shop from another script entry not yet]**.
    One scale for every area (smaller only when an area doesn't fit), so rooms compare. **[done]**
  - **Header line**: location name, gil, play time. **[done]**
  - **Compact party strip**: four rows of portrait, HP and MP, status icons. **[done]** (portraits read from the
    menu's file by the port)
  - **Large edge buttons** (≥ 40×32): **Menu** (also △) **[done]**; **ATE**, pulsing when an Active Time Event is
    available (Select stays Select) **[done: the script's flag (R30); not yet seen with a real ATE]**; **Cards** with the collection count, opening the card screen **[done]**; **Mognet**
    with the letters carried, at a moogle **[done: shown when the Moogle is what Cross would talk to; a tap talks to it]**; **Chocograph / hints** when relevant **[todo]**.
  - Nothing here is needed while walking. The party and MENU show only while the game allows the menu (the opening
    and cutscene stretches may hold a placeholder party); CARDS once the card game has been met. **[done]**
  - The map is the walkmesh area connected to where the player stands (other parts are other screens), scaled to
    fill its box. **[done]**
  - The maps are baked at build time from the discs (`make maps`: tools/bake_maps.py over the decomp's offline field
    reader, decomp R39) into build/maps.bin, packed beside the disc pack: every exit the field's script has, whatever
    the story state; walkable as stored; save moogles and shops where the script places them. Without maps.bin the
    game builds each map as a field loads. **[done; per-story-state variants for the 179 fields whose walkable area
    changes (doors, bridges) not yet]**

## Options
- The port's settings: 3D depth (low / normal / high, on top of the slider), top screen (fit: scaled to the screen's
  height; sharp: the PS1's pixels 1:1), voices (on / off). Kept in sdmc:/3ds/ff9/settings.cfg (port/3ds/settings.c).
  Three ways in: the title screen's OPTIONS button; 3DS OPTIONS beside the Config screen's title (the panel over the
  screen, BACK returns); the field screen's LOG view, OPTIONS in the log's place. **[done]**

## Title
- **Top**: the title picture and logo only (the PS1's NEW GAME / CONTINUE and hand are not drawn there). **Bottom**:
  NEW GAME, CONTINUE (dim with no save) and OPTIONS as large buttons, the D-pad's choice marked; a tap acts directly
  (the game's cursor set, its own decide). **[done: port/game/title_ui.c]**

## Dialogue
- The text stays on top in FF9's speaker-anchored bubbles, at the screen plane. **[done]**
- The bottom can show a scrollable **dialogue backlog** (new). **[done in fields: LOG swaps it in for the map and
  party; taps on its upper or lower half scroll]**
- **Choice prompts**: the choices as large touch buttons on the bottom while the question stays on top; cursor and
  A still work (an input aid, not a mirror). **[done in fields: the opening's candle question, tapped; also in
  cutscenes, where the rest of the bottom screen waits for player control; the end of a disc's "save your game?"
  (the field's own disc-change prompt, port/game/disc_prompt.c)]**
- **Disc change**: none (every disc is in the one ROM): the disc-change screen puts the disc in and runs its own check
  at once, no prompt. **[done: port/game/disc_change.c; verified from the end of disc 1 to disc 2's field 800]**

## Battle
(Touch acts directly, never by simulated presses: a tap is applied at the game's next input step for that menu,
with the cursor set and the game's own confirm run. port/game/battle_ui.c.)
- **Top**: the battle scene with no command or status windows **[done]**; damage and heal numbers **[done]**; the
  ability-name banner **[done]**; the PS1 target hand in the scene **[done]**; a "ready" marker on the character
  whose turn it is **[done: the game's own]**; no ATB pips (tried, removed at the play-tester's request: the cards on the bottom have the gauges).
- **Bottom**:
  - **Left ~45%: command list**, large rows (Attack, Steal/Skill, Defend, Item, Change/Run); sub-lists (abilities,
    items) replace it in place with MP cost and count, and a description line at the bottom (the PS1 help text).
    **[done: large rows, DEFEND / CHANGE; ability / item / throw lists as large rows drawn by the game's own row code
    with the entry's description and BACK (port/game/battle_ui.c)]**
  - **Right: four party cards**: name, HP, MP, ATB bar and Trance bar, status icons; tapping a ready character's
    card switches to them (PS1 △/✕). **[done: portrait, name, HP, MP, ATB, Trance, status icons, a ready card's tap]**
- **Targeting**: the command area becomes an **enemy/ally panel laid out like the formation**, a large name button
  per enemy (HP once Libra was used); tap selects, tap again confirms; a **single / all** toggle for R; the top
  cursor follows; physical controls drive the same state. **[done: ENEMY / PARTY buttons (party with HP), ALL / ONE,
  BACK, for both kinds of targeting; a tap targets that unit and confirms at once]**
- Wait mode: opening a sub-list pauses ATB exactly as on PS1; no timing changes. **[done: game logic unchanged]**
- **Rewards**: the PS1's reward windows on the bottom (A advances them as before); the top shows all of it at once: a
  card per member (portrait, level, an EXP bar to the next level, EXP and AP gained, LEVEL UP, abilities learned) and
  the gil, items and card won, read from the rewards screen's own state as it counts. **[done: port/game/rewards_top.c]**

## Main menu
- **Bottom**: the PS1 menu with touch zones. **[partial: 1:1; direct taps on the main menu (menu_touch.c), every list
  drawn with the game's list box (list_touch.c: Item, Ability, Equip candidates, shops, ...) and the Item screen's member
  window, Equip's slots, Ability's list (also from its tabs), tabs and use-on window (equip_touch.c), Config's options
  (config_touch.c), Status (edges switch member, the middle opens the abilities: status_touch.c)]**
- **Top**, per sub-menu: Equip: a rotating 3D model in the selected gear, before→after stats; Abilities: AP progress
  for every ability the gear teaches; Status: full stat sheet and a large portrait; Items: description and who can
  use it. Game logic pauses; the top shows the paused scene dimmed to half, so opening the menu (on the bottom) visibly
  changes the top too (play-test 2026-10-31: with the top untouched, nothing there said the game was in a menu). **[partial (port/game/menu_top.c), only what the bottom doesn't show:
  Items: for equipment, every member, whether they can wear it and what it changes for them; Equip: all of a slot's
  candidates at once with their changes and the abilities each teaches with the member's AP (seen: weapon list); Abilities: which equipment teaches it. The 3D model: the member's
  battle model turning in its idle with the weapon held, while the slots are shown (port/game/model_view.c, R35); otherwise the top keeps the paused scene, dimmed; on a screen with views, an entry without one shows the empty
  panel]**

- **Select help**: the text on the top screen, large, along its foot; the moogle stays by the cursor on the bottom
  (the PS1's bubble covered the menu it explains). **[done]**

## Shops and synthesis
- **Bottom**: item list, a quantity stepper (large +/-, L/R ×10), Buy/Sell tabs. **[partial: the PS1 shop 1:1]**
- **Top**: the "who can equip ↑/↓" party panel large, the stat change for the highlighted character, the abilities
  the item teaches; synthesis adds ingredients and how many you own. **[partial: the item, its description, who can
  wear it with before → after, and the abilities it teaches each member with their AP towards them (all shops and the
  Item screen). Ingredients stay on the bottom: the PS1's synthesis window already lists them and marks the missing]**

## Save screens (moogle)
- **Bottom**: large slot buttons. **[partial: the PS1 save screen 1:1; taps on card slots, files and Yes/No act
  directly (save_touch.c)]**
- **Top**: the selected slot's details (party, location, time, gil). **[dropped: the PS1's file list already shows
  them; the top keeps the paused scene]**
- Overwriting needs a separate confirmation, never one tap. **[done: the game's own confirm]**

## World map
- **Top**: the 3D world without the minimap. **[done]**
- **Bottom**: a large world map with the player or vehicle arrow and discovered places **[done]**; tapping a place
  shows its name **[done]**; a zoom toggle **[done: a tap zooms 2x with the party in the middle, and back]**; in chocobo mode the Chocograph and dig hints **[todo]**.
- Vehicle controls stay physical. **[done]**

## Tetra Master
- **Bottom**: the game's own frame 1:1, the play surface (board, hands, the stock grid, its Yes/No windows). Touch acts
  on it directly: a tap on a stock card picks it, on a hand card takes it up, on a square places the card taken up (or
  the one under the cursor), on Yes/No answers, on a captured card takes it, anywhere continues a result.
  **[done: port/game/tm_ui.c; the cursor fields set at the game's frame and its own confirm run (decomp R33)]**
- **Top**: the card in focus large (picture, name, the "1P2A" string, arrows), the score and whose turn, and the game's
  hint text (its bubbles covered the board). **[done; no opponent portrait: the PS1 has none]**
- D-pad and A still place cards. **[done]**

## Mognet
- Mognet is the field's dialogue (ff9-decomp R34): the Moogle's menu, Mognet's menu, the letter list and the letter
  are message windows, so the letter reads on the top screen in the game's window and every choice is a bottom-screen
  button acting directly (a choice the script polls gets its Cross at the frame's object update). **[done]**

## FMVs
- **Top**: the video at the screen plane, realtime overlays with it. **[done]**
- **Bottom**: black or a static location card. **[done: black]**
