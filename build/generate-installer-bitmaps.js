#!/usr/bin/env node


const fs = require('fs');
const path = require('path');


const COLORS = {
  bg:        [0x08, 0x06, 0x0E],
  bgLight:   [0x14, 0x10, 0x20],
  purple:    [0xA7, 0x8B, 0xFA],
  purpleDim: [0x7C, 0x3A, 0xED],
  accent:    [0xE8, 0x79, 0xF9],
  white:     [0xFF, 0xFF, 0xFF],
  textDim:   [0xA0, 0x9A, 0xAD],
};

function createBMP(width, height, pixelCallback) {
  const rowSize = Math.ceil((width * 3) / 4) * 4;
  const pixelDataSize = rowSize * height;
  const fileSize = 54 + pixelDataSize;

  const buf = Buffer.alloc(fileSize, 0);


  buf.write('BM', 0);                       
  buf.writeUInt32LE(fileSize, 2);               
  buf.writeUInt32LE(0, 6);                      
  buf.writeUInt32LE(54, 10);                  


  buf.writeUInt32LE(40, 14);                  
  buf.writeInt32LE(width, 18);                 
  buf.writeInt32LE(height, 22);                
  buf.writeUInt16LE(1, 26);                   
  buf.writeUInt16LE(24, 28);                  
  buf.writeUInt32LE(0, 30);                     
  buf.writeUInt32LE(pixelDataSize, 34);         
  buf.writeInt32LE(2835, 38);                  
  buf.writeInt32LE(2835, 42);                  
  buf.writeUInt32LE(0, 46);                    
  buf.writeUInt32LE(0, 50);                    


  for (let y = 0; y < height; y++) {
    const bmpY = height - 1 - y;
    for (let x = 0; x < width; x++) {
      const [r, g, b] = pixelCallback(x, y, width, height);
      const offset = 54 + bmpY * rowSize + x * 3;
      buf[offset] = b;     
      buf[offset + 1] = g;  
      buf[offset + 2] = r;  
    }
  }

  return buf;
}

function lerp(a, b, t) {
  return Math.round(a + (b - a) * t);
}

function lerpColor(c1, c2, t) {
  return [lerp(c1[0], c2[0], t), lerp(c1[1], c2[1], t), lerp(c1[2], c2[2], t)];
}

function clamp(v, min, max) {
  return Math.max(min, Math.min(max, v));
}


function generateSidebar() {
  return createBMP(164, 314, (x, y, w, h) => {
   
    const gradT = y / h;
    let color = lerpColor(COLORS.bg, COLORS.bgLight, gradT * 0.6);

   
    const glowCenterX = w * 0.4;
    const glowCenterY = h * 0.25;
    const dx = (x - glowCenterX) / w;
    const dy = (y - glowCenterY) / h;
    const glowDist = Math.sqrt(dx * dx + dy * dy);
    const glowIntensity = Math.max(0, 1 - glowDist * 2.5) * 0.18;
    color = lerpColor(color, COLORS.purpleDim, glowIntensity);

   
    const glow2CenterX = w * 0.6;
    const glow2CenterY = h * 0.75;
    const dx2 = (x - glow2CenterX) / w;
    const dy2 = (y - glow2CenterY) / h;
    const glow2Dist = Math.sqrt(dx2 * dx2 + dy2 * dy2);
    const glow2Intensity = Math.max(0, 1 - glow2Dist * 2.5) * 0.1;
    color = lerpColor(color, COLORS.accent, glow2Intensity);

  
    if (x >= w - 2) {
      const lineAlpha = 0.25;
      color = lerpColor(color, COLORS.purple, lineAlpha);
    }

  
    const gemCX = w / 2;
    const gemCY = h * 0.35;
    const gemSize = 28;
    const gemDX = Math.abs(x - gemCX);
    const gemDY = Math.abs(y - gemCY);

    const inGem = (gemDX / gemSize + gemDY / (gemSize * 1.3)) < 1 && gemDY < gemSize * 1.1;
    if (inGem) {
      const gemT = 1 - (gemDX / gemSize + gemDY / (gemSize * 1.3));
      color = lerpColor(COLORS.purpleDim, COLORS.accent, y < gemCY ? 0.3 : 0.7);
  
      if (gemDY < gemSize * 0.3 && gemDX < gemSize * 0.4) {
        color = lerpColor(color, COLORS.white, 0.15);
      }
    }

   
    const gemGlowDist = Math.sqrt(Math.pow((x - gemCX) / 40, 2) + Math.pow((y - gemCY) / 50, 2));
    if (gemGlowDist < 1 && !inGem) {
      const gemGlowAlpha = (1 - gemGlowDist) * 0.15;
      color = lerpColor(color, COLORS.purple, gemGlowAlpha);
    }

  
    if (y >= h * 0.48 && y <= h * 0.52) {
      const textAlpha = 0.06;
      color = lerpColor(color, COLORS.white, textAlpha);
    }

    // Subtle grid/dots pattern
    if (x % 20 === 0 && y % 20 === 0) {
      color = lerpColor(color, COLORS.purple, 0.08);
    }

    return color.map(c => clamp(c, 0, 255));
  });
}


function generateHeader() {
  return createBMP(150, 57, (x, y, w, h) => {
  
    const gradT = x / w;
    let color = lerpColor(COLORS.bg, COLORS.bgLight, gradT * 0.5);

    
    const glowDist = x / w;
    const glowAlpha = Math.max(0, 1 - glowDist * 1.5) * 0.2;
    color = lerpColor(color, COLORS.purpleDim, glowAlpha);

 
    const accentDist = (w - x) / w;
    const accentAlpha = Math.max(0, 1 - accentDist * 2) * 0.1;
    color = lerpColor(color, COLORS.accent, accentAlpha);

 
    const gemCX = 25;
    const gemCY = h / 2;
    const gemS = 10;
    const gDX = Math.abs(x - gemCX);
    const gDY = Math.abs(y - gemCY);
    if ((gDX / gemS + gDY / (gemS * 1.2)) < 1) {
      color = lerpColor(COLORS.purpleDim, COLORS.accent, gDY / gemS);
    }

  
    if (y >= h - 2) {
      const lineGrad = x / w;
      color = lerpColor(COLORS.purpleDim, COLORS.accent, lineGrad);
      color = lerpColor(color, COLORS.bg, 0.3);
    }

    return color.map(c => clamp(c, 0, 255));
  });
}

function generateUninstallerSidebar() {
  return createBMP(164, 314, (x, y, w, h) => {
    const gradT = y / h;
    let color = lerpColor(COLORS.bg, COLORS.bgLight, gradT * 0.4);


    const glowCX = w * 0.5;
    const glowCY = h * 0.35;
    const dx = (x - glowCX) / w;
    const dy = (y - glowCY) / h;
    const dist = Math.sqrt(dx * dx + dy * dy);
    const intensity = Math.max(0, 1 - dist * 2.5) * 0.12;
    color = lerpColor(color, [0xF8, 0x71, 0x71], intensity);

    // Right edge line
    if (x >= w - 2) {
      color = lerpColor(color, [0xF8, 0x71, 0x71], 0.2);
    }

    return color.map(c => clamp(c, 0, 255));
  });
}


const buildDir = path.join(__dirname);

fs.writeFileSync(path.join(buildDir, 'installerSidebar.bmp'), generateSidebar());
console.log('✓ Created installerSidebar.bmp (164x314)');

fs.writeFileSync(path.join(buildDir, 'installerHeader.bmp'), generateHeader());
console.log('✓ Created installerHeader.bmp (150x57)');

fs.writeFileSync(path.join(buildDir, 'uninstallerSidebar.bmp'), generateUninstallerSidebar());
console.log('✓ Created uninstallerSidebar.bmp (164x314)');


const iconSrc = path.join(__dirname, '..', 'assets', 'icon.ico');
const iconDest = path.join(buildDir, 'icon.ico');
if (fs.existsSync(iconSrc) && !fs.existsSync(iconDest)) {
  fs.copyFileSync(iconSrc, iconDest);
  console.log('✓ Copied icon.ico to build/');
}

console.log('\nAll installer assets generated successfully!');
