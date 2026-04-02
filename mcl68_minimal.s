* Minimal 68K test - vasm Motorola syntax
* Reset vectors
  ORG $00000
  dc.l $000003F0   * Vector 0: SP
  dc.l $00000400   * Vector 1: PC

* Code at $400
  ORG $000400
  move.l #$000003F0,a7
  bra *
  END
