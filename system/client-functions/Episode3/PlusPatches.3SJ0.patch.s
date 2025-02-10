# Original patch by Gigobooma
# https://docs.google.com/document/d/1zG73l9joEqp_zB-xNgK9g8pXL0RSpmXfxPFQcdAvess/edit

.meta name="Episode 3 Plus"
.meta description="Enables Episode 3\nPlus features\n\nOriginally created\nby Gigobooma"

entry_ptr:
reloc0:
  .offsetof start

start:
  .include  WriteCodeBlocksGC

  # Disable curse word filter
  .data     0x8012FAD0
  .data     0x00000004
  nop

  # 16-bit deckbuilder card IDs
  .data     0x80278BC4
  .data     0x00000004
  lhzx      r0, [r27 + r29]
  .data     0x80278C28
  .data     0x00000004
  addi      r29, r29, 2
  .data     0x8027FD60
  .data     0x00000008
  lhzx      r0, [r5 + r3]
  addi      r3, r3, 2
  .data     0x80280308
  .data     0x00000004
  lhzx      r17, [r25 + r28]
  .data     0x80280520
  .data     0x00000004
  addi      r28, r28, 0x0002

  # Replace deckbuilder card list
  .data     0x80580E68
  .data     0x00000004
  .data     0x00000264
  .data     0x80427578
  .deltaof  deckbuilder_cards_start, deckbuilder_cards_end
deckbuilder_cards_start:
  .binary   0009000F02460247015501530243024401690122015700280164024C024D0158
  .binary   024A024802490168024B016E017A002C0252025101650250015A0159024E0167
  .binary   024F00140015016F015E0029025A025B01660253025400100256002002570255
  .binary   02590258000B025D014F025E0161015B025C01700160000D025F02600177002D
  .binary   016C02610179016B016D0267000E02620162026302640172015F017800250266
  .binary   0265015401560012017501760268016A017B0173026A02690174026B001C000A
  .binary   0150027002710038026C0186026D026E026F01230034017C027802740277017F
  .binary   02790180027302720184027502760181003A0026015C000C027D0151027E0171
  .binary   027C027A027B018200350283017D002102840011028201870039028001850281
  .binary   027F017E018C0285018801890183018B018A0040018D0289028A028801910286
  .binary   001F0287019B01A1004201A0028E018E028B028C0290028F028D001D019C019D
  .binary   0190004601990124029200410293018F0294019A019E02910193004501920163
  .binary   0194019701950198019601A3001701A4001E002301A501A90034029701A801A7
  .binary   01AC029501AA002201AB029600240016003001C3003D01AE01AF004901AD01B0
  .binary   01B101B201B301C101B801BA01BB01B901C401BD01B601C001BC01BE01BF01C2
  .binary   01C502A301B401B500560057005801C601C701C80059005A01CD01CE005B0031
  .binary   005C01C901CA01E401F801FA0037005E005D01CB01CC01E501E601E701E8005F
  .binary   006001CF01D001E9002A00610062006301D101D201D30064006501D401D50069
  .binary   006A0068006B006C0066001B01D60067003601EA01EB01EE01EC01FB01FC006D
  .binary   006E020001D901D70201006F007001DC01DD01F1002701F200710032007201DA
  .binary   01DB01EF01F001F301F4007301DE01FF00740075007601DF01E001E100780079
  .binary   007A007C007D007B01E300330077007E01E2007F003B01D801F501F601FE01FD
  .binary   01F7008A008B008C008D0093009400950099009A009C008E00B9008F00B200B7
  .binary   009D00AA00B1003C00B800C000F400AC00BA00C1020B020C020D020E020F0210
  .binary   02110212021300900092009100B600A7009E003E00A200A300B400AE00BC0214
  .binary   021602060207021800A000A400B300A100C4020202030204021700A500A60205
  .binary   00AB00BB0208020900C300970098020A00AD00D800D900DA00DB00DC00DD00DE
  .binary   00DF00E000E100E200E400E500E600E800EB00E700E900EA00EC00ED00EE00EF
  .binary   00F000F100F2021500F300C600C500CB021C023400CA00D2023B00D500CC021A
  .binary   021B00D402190298023300D100C7023E00D6023D023C00CF0227003F02260228
  .binary   022A0238021E021F0220022102220223022402300232022F023100D0022C022E
  .binary   021D02370239023A022502290043022B0235023600F500F600F700F6010300F9
  .binary   0101010201000242010B00F8014E0129010A01250126012801270108012A0138
  .binary   0139013A00FA00FB013D013E013F01400141014201430144014501460104010E
  .binary   00FC012D012E012F014800FD00FE013000FF0132014D01050106012101370241
  .binary   0107010D013C010F013501090136023F013B014A0019010C014B012B001A0131
  .binary   012C0133014C01340240024500130152019F002E01A601B701A202C002C102C2
  .binary   02C302C402C502C6002F01ED01F902BC02BD02C802CA02C902C702CB029A0299
  .binary   02A2002B00440018
deckbuilder_cards_end:

  # Disable Ep3PlayerDataSegment_remove_invalid_cards
  .data     0x802A03DC
  .data     0x00000004
  blr

  # Always get 10 cards after any battle
  .data     0x802C1380
  .data     0x00000008
  nop
  li        r5, 10

  # Disable verify_deck_contents
  .data     0x8030979C
  .data     0x00000008
  li        r3, 0
  blr

  # Add Booooo and Laughter sound chats
  .data     0x8030A538
  .data     0x00000004
  cmpwi     r3, 41
  .data     0x8030A544
  .data     0x00000004
  rlwinm    r0, r3, 1, 0, 30
  .data     0x8030A550
  .data     0x00000004
  lhzx      r3, [r3 + r0]
  .data     0x8030A6D0
  .data     0x00000004
  li        r4, 0x0029
  .data     0x8030A70C
  .data     0x00000004
  cmpwi     r31, 41
  .data     0x8042A4C8
  .deltaof  sound_chat_sound_ids_start, sound_chat_sound_ids_end
sound_chat_sound_ids_start:
  .binary   802580268227852D803080318A3F85328A4085338A418A288A388A298A39852E
  .binary   802F853D85348535853B85368537852B853A853C853E80448045804680478048
  .binary   8049804A804B804C804D804E804F802A802C0000
sound_chat_sound_ids_end:

  # Change default starting cards
  .data     0x804225B0
  .deltaof  starting_cards_start, starting_cards_end
starting_cards_start:
  .binary   00090009000A000A000B000C000C00290029002C003500400040004100420042
  .binary   01A301A300170017002800280034003400160151018F02460056005600590059
  .binary   005A005B005B005F005F006400640078007800610061008A008A008C008C00A3
  .binary   0093008E00D800D800DB00DE00C500C500C600C600CC00010007000400080110
  .binary   01160113011700020118000501190006011A0114011B0003011C0111011D0112
  .binary   011E0115011F02B202B302B402B502AA02AB02AC02AD02AE02AF02B002B10050
  .binary   00510052005300470048004A004B004C004D004E004F0000
starting_cards_end:

  .data     0x00000000
  .data     0x00000000
