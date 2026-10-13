from pathlib import Path
import cairosvg
p=Path('TaskPanel/Resources/lib/ui')
def save(name,w,h,body):
 svg=f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}"><defs><linearGradient id="gold" x2="0" y2="1"><stop stop-color="#ffe59a"/><stop offset=".5" stop-color="#d5a244"/><stop offset="1" stop-color="#8c5b29"/></linearGradient><linearGradient id="paper" x2="0" y2="1"><stop stop-color="#fff1ce"/><stop offset="1" stop-color="#ead29d"/></linearGradient></defs>{body}</svg>'
 (p/(name+'.svg')).write_text(svg);cairosvg.svg2png(bytestring=svg.encode(),write_to=str(p/(name+'.png')))
save('panel',96,96,'<rect x="3" y="5" width="90" height="90" rx="16" fill="#332719" opacity=".5"/><rect x="2" y="2" width="92" height="92" rx="16" fill="url(#gold)"/><rect x="7" y="7" width="82" height="82" rx="12" fill="#86502f"/><rect x="11" y="11" width="74" height="74" rx="9" fill="url(#paper)"/><rect x="14" y="14" width="68" height="68" rx="7" fill="none" stroke="#bc9454" stroke-opacity=".6"/><path d="M13 25V19Q13 13 19 13H25 M71 13H77Q83 13 83 19V25 M13 71V77Q13 83 19 83H25 M71 83H77Q83 83 83 77V71" fill="none" stroke="#fff3c1" stroke-width="2"/>')
save('button',96,48,'<rect x="2" y="4" width="92" height="43" rx="14" fill="#6b4229"/><rect x="2" y="1" width="92" height="43" rx="14" fill="url(#gold)"/><rect x="6" y="5" width="84" height="35" rx="10" fill="url(#paper)"/><path d="M15 8H81" stroke="#fff8e3" stroke-width="2"/>')
save('close',48,48,'<circle cx="24" cy="26" r="21" fill="#6c3826"/><circle cx="24" cy="23" r="21" fill="#f9d37c"/><circle cx="24" cy="23" r="17" fill="#b94636"/><path d="M18 17L30 29 M30 17L18 29" stroke="#fff2d7" stroke-width="4" stroke-linecap="round"/>')
save('book',64,64,'<rect x="4" y="5" width="56" height="56" rx="16" fill="#4b3123"/><rect x="4" y="2" width="56" height="56" rx="16" fill="url(#gold)"/><rect x="8" y="6" width="48" height="48" rx="12" fill="#795038"/><path d="M13 17Q23 13 32 19Q42 13 51 17V43Q41 40 32 46Q23 40 13 43Z" fill="url(#paper)" stroke="#fbe6a3" stroke-width="1.5"/><path d="M32 19V46 M18 23L27 24 M18 29L27 30 M18 35L27 36 M37 24L46 23 M37 30L46 29 M37 36L46 35" stroke="#b38b52" stroke-width="1.4"/><path d="M43 15V31L47 27L51 30V16" fill="#b94636"/>')
save('white',1,1,'<rect width="1" height="1" fill="white"/>')
print('5 SVG-authored PNG assets generated')
