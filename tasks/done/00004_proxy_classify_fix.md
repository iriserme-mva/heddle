# Proxy classify() reserved‑word fix

**Status:** Done  
**Version:** 0.1.13 (2026‑10‑24)  

**Description:** Fixed `classify()` mapping so reserved host commands `/chain/quit` and `/chain/status` are correctly routed. Added regression test.  
**References:** STATUS.md entry 2026‑10‑12‑13.  
**Result:** CI green; host now quits correctly.
