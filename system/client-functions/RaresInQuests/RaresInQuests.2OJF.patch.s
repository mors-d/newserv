.meta name="Rares in quests"
.meta description="Disables logic that\nprevents items\nabove 8 stars and\nrares from dropping\nin quests."

entry_ptr:
reloc0:
  .offsetof start
start:
  .include  WriteCodeBlocksDC

  .align    4
  .data     0x8C2192C8
  .data     2
  nop

  .align    4
  .data     0x8C219254
  .data     2
  nop

  .align    4
  .data     0x00000000
  .data     0x00000000
