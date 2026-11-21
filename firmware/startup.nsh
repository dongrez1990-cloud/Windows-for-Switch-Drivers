@echo -off
echo "[nsh] begin"
if exist fs0:\EFI\Microsoft\Boot\bootmgfw.efi then
  echo "[nsh] fs0 launching"
  fs0:\EFI\Microsoft\Boot\bootmgfw.efi
  echo "[nsh] fs0 returned"
endif
if exist fs1:\EFI\Microsoft\Boot\bootmgfw.efi then
  echo "[nsh] fs1 launching"
  fs1:\EFI\Microsoft\Boot\bootmgfw.efi
  echo "[nsh] fs1 returned"
endif
echo "[nsh] listing fs0 root:"
dir fs0:\
echo "[nsh] done"
