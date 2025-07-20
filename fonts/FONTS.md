<img width="320" height="241" alt="image" src="https://github.com/user-attachments/assets/ee26b934-49c1-4dcd-95b5-8bbf093c84e4" />


# one‑time setup
sudo apt install python3-venv unzip wget fontconfig
python3 -m venv ~/venvs/fonttools
source ~/venvs/fonttools/bin/activate
pip install "fonttools[woff]"

# download & extract font
wget -O JetBrainsMono.zip \
  https://github.com/ryanoasis/nerd-fonts/releases/download/v3.2.1/JetBrainsMono.zip
unzip -o JetBrainsMono.zip -d JetBrainsMono
cd JetBrainsMono


pyftsubset JetBrainsMonoNerdFontMono-Regular.ttf \
  --output-file=osd.ttf \
  --unicodes="U+0020-007E,U+F012,U+F2C8,U+F240" \
  --layout-features="*" --glyph-names


If you need a compressed webfont later
pyftsubset JetBrainsMonoNerdFontMono-Regular.ttf \
  --flavor=woff2 \
  --output-file=osd.woff2 \
  --unicodes="U+0020-007E,U+F012,U+F2C8,U+F240" \
  --layout-features="*" --glyph-names






# keep ASCII, RSSI, and add everything in the table above
UNICODES="U+0020-007E,\
U+F012,\
U+F015,U+F041,U+F05B,U+F071,U+F072,U+F0E7,U+F106,U+F107,\
U+F1EB,\
U+F240,U+F241,U+F242,U+F243,U+F244,\
U+F2C7,U+F2C8,U+F2C9,U+F2CA,U+F2CB"

# output a plain TTF (libschrift can’t read WOFF2 directly)
pyftsubset JetBrainsMonoNerdFontMono-Regular.ttf \
  --output-file=osd.ttf \
  --unicodes="$UNICODES" \
  --layout-features="*" --glyph-names






echo -e '&F99
\xEF\x80\x92\xEF\x87\xAB\xEF\x80\x95\xEF\x81\x81\xEF\x81\x9B
\xEF\x81\xB1\xEF\x81\xB2\xEF\x83\xA7\xEF\x84\x86\xEF\x84\x87
\xEF\x89\x80\xEF\x89\x81\xEF\x89\x82\xEF\x89\x83\xEF\x89\x84
\xEF\x8B\x87\xEF\x8B\x88\xEF\x8B\x89\xEF\x8B\x8A\xEF\x8B\x8B' \
> /tmp/MSPOSD.msg


