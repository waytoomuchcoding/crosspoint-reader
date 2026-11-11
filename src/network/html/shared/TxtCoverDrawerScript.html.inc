let txtCoverBmpB64 = '';
let txtDefaultStemTitle = '';
let txtDefaultStemAuthor = '';

function isTxtOrMdFilename(name) {
  const lower = (name || '').toLowerCase();
  return lower.endsWith('.txt') || lower.endsWith('.md');
}

function parseStemTitleAuthor(filename) {
  const dot = filename.lastIndexOf('.');
  const stem = (dot > 0 ? filename.slice(0, dot) : filename).trim();
  const cjkOpen = stem.indexOf('《');
  const cjkClose = stem.indexOf('》', cjkOpen + 1);
  if (cjkOpen === 0 && cjkClose > 1) {
    const innerTitle = stem.slice(1, cjkClose).trim();
    const tailAuthor = stem.slice(cjkClose + 1).replace(/^[\s\-—–_·:：]+/, '').trim();
    if (innerTitle) return { title: innerTitle, author: tailAuthor };
  }
  for (const sep of [' - ', ' — ', ' – ']) {
    const idx = stem.lastIndexOf(sep);
    if (idx > 0) {
      const t = stem.slice(0, idx).trim();
      const a = stem.slice(idx + sep.length).trim();
      if (t && a) return { title: t, author: a };
    }
  }
  return { title: stem, author: '' };
}

function drawProceduralTxtCanvasPreview() {
  const canvas = document.getElementById('txtCoverPreviewCanvas');
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  const w = 148, h = 222, spineW = 14;
  ctx.fillStyle = '#ffffff';
  ctx.fillRect(0, 0, w, h);
  ctx.fillStyle = '#000000';
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < spineW; x++) {
      if ((x + y) % 2 === 0) ctx.fillRect(x, y, 1, 1);
    }
  }
  ctx.fillRect(spineW, 0, 2, h);
  ctx.fillStyle = '#ffffff';
  for (let sy = 18; sy < h - 14; sy += 28) ctx.fillRect(3, sy, spineW - 6, 2);
  ctx.fillStyle = '#000000';
  for (let y = 6; y < h - 6; y += 6) {
    for (let x = spineW + 6; x < w - 6; x += 6) ctx.fillRect(x, y, 1, 1);
  }
  const px0 = spineW + 8, py0 = 14, pw = w - spineW - 16, ph = h - 48;
  ctx.fillStyle = '#ffffff';
  ctx.fillRect(px0, py0, pw, ph);
  ctx.strokeStyle = '#000000';
  ctx.lineWidth = 2;
  ctx.strokeRect(px0 + 1, py0 + 1, pw - 2, ph - 2);
  ctx.lineWidth = 1;
  ctx.strokeRect(px0 + 4.5, py0 + 4.5, pw - 9, ph - 9);
  ctx.fillStyle = '#000000';
  ctx.fillRect(px0 + 8, py0 + 8, pw - 16, 16);
  ctx.fillStyle = '#ffffff';
  ctx.font = 'bold 10px sans-serif';
  ctx.textAlign = 'center';
  ctx.fillText('TXT', px0 + pw / 2, py0 + 19);
  const title = (document.getElementById('txtMetaTitle')?.value || txtDefaultStemTitle || 'Untitled').trim();
  const author = (document.getElementById('txtMetaAuthor')?.value || '').trim();
  ctx.fillStyle = '#000000';
  ctx.font = 'bold 12px sans-serif';
  const line1 = title.slice(0, 10);
  const line2 = title.slice(10, 20);
  ctx.fillText(line1, px0 + pw / 2, py0 + 48);
  if (line2) ctx.fillText(line2, px0 + pw / 2, py0 + 66);
  if (author) {
    ctx.fillStyle = '#ffffff';
    ctx.fillRect(px0, h - 26, pw, 18);
    ctx.strokeRect(px0 + 0.5, h - 25.5, pw - 1, 17);
    ctx.fillStyle = '#000000';
    ctx.font = '10px sans-serif';
    ctx.fillText(author.slice(0, 12), px0 + pw / 2, h - 13);
  }
}

function renderTxtCoverProceduralIfEmpty() {
  if (!txtCoverBmpB64) drawProceduralTxtCanvasPreview();
}

function encode1BitTopDownBmpBase64(bitsWhite, w, h) {
  const rowBytes = ((w + 31) >> 5) << 2;
  const imageSize = rowBytes * h;
  const headerSize = 62;
  const buf = new Uint8Array(headerSize + imageSize);
  const dv = new DataView(buf.buffer);
  buf[0] = 0x42; buf[1] = 0x4D; // 'BM'
  dv.setUint32(2, headerSize + imageSize, true);
  dv.setUint32(10, headerSize, true);
  dv.setUint32(14, 40, true);
  dv.setInt32(18, w, true);
  dv.setInt32(22, -h, true); // top-down
  dv.setUint16(26, 1, true);
  dv.setUint16(28, 1, true); // 1-bit
  dv.setUint32(34, imageSize, true);
  dv.setUint32(38, 2835, true);
  dv.setUint32(42, 2835, true);
  dv.setUint32(46, 2, true);
  dv.setUint32(50, 2, true);
  // Palette 0 = black (0x00000000), Palette 1 = white (0x00FFFFFF)
  dv.setUint32(54, 0x00000000, true);
  dv.setUint32(58, 0x00FFFFFF, true);
  for (let y = 0; y < h; y++) {
    const rowOff = headerSize + y * rowBytes;
    for (let x = 0; x < w; x++) {
      if (bitsWhite[y * w + x]) {
        buf[rowOff + (x >> 3)] |= (1 << (7 - (x & 7)));
      }
    }
  }
  let binary = '';
  for (let i = 0; i < buf.length; i++) binary += String.fromCharCode(buf[i]);
  return btoa(binary);
}

function processTxtCoverImageFile(imgFile) {
  if (!imgFile || !imgFile.type.startsWith('image/')) return;
  const reader = new FileReader();
  reader.onload = function(ev) {
    const img = new Image();
    img.onload = function() {
      const w = 148, h = 222;
      const canvas = document.getElementById('txtCoverPreviewCanvas');
      const ctx = canvas.getContext('2d');
      const targetAspect = w / h;
      const srcAspect = img.width / img.height;
      let sx = 0, sy = 0, sw = img.width, sh = img.height;
      if (srcAspect > targetAspect) {
        sw = Math.round(img.height * targetAspect);
        sx = Math.floor((img.width - sw) / 2);
      } else {
        sh = Math.round(img.width / targetAspect);
        sy = Math.floor((img.height - sh) / 2);
      }
      ctx.fillStyle = '#ffffff';
      ctx.fillRect(0, 0, w, h);
      ctx.drawImage(img, sx, sy, sw, sh, 0, 0, w, h);
      const imgData = ctx.getImageData(0, 0, w, h);
      const d = imgData.data;
      const lum = new Float32Array(w * h);
      for (let i = 0; i < w * h; i++) {
        const r = d[i * 4], g = d[i * 4 + 1], b = d[i * 4 + 2], a = d[i * 4 + 3] / 255;
        const gray = (0.299 * r + 0.587 * g + 0.114 * b) * a + 255 * (1 - a);
        lum[i] = Math.max(0, Math.min(255, (gray - 128) * 1.22 + 128));
      }
      const bitsWhite = new Uint8Array(w * h);
      for (let y = 0; y < h; y++) {
        const row = y * w;
        for (let x = 0; x < w; x++) {
          const oldVal = Math.max(0, Math.min(255, lum[row + x]));
          const isWhite = oldVal >= 128 ? 1 : 0;
          const newVal = isWhite ? 255 : 0;
          bitsWhite[row + x] = isWhite;
          const err = (oldVal - newVal) / 8;
          if (x + 1 < w) lum[row + x + 1] += err;
          if (x + 2 < w) lum[row + x + 2] += err;
          if (y + 1 < h) {
            if (x > 0) lum[row + w + x - 1] += err;
            lum[row + w + x] += err;
            if (x + 1 < w) lum[row + w + x + 1] += err;
          }
          if (y + 2 < h) lum[row + 2 * w + x] += err;
          const pxIdx = (row + x) * 4;
          d[pxIdx] = d[pxIdx + 1] = d[pxIdx + 2] = newVal;
          d[pxIdx + 3] = 255;
        }
      }
      ctx.putImageData(imgData, 0, 0);
      txtCoverBmpB64 = encode1BitTopDownBmpBase64(bitsWhite, w, h);
      document.getElementById('txtCoverClearBtn').style.display = 'inline-block';
      document.getElementById('txtCoverPreviewCaption').textContent = '1-Bit Bound Cover';
      document.getElementById('txtCoverDropLabel').textContent = '✓ Cover ready: ' + (imgFile.name || 'clipboard image');
    };
    img.src = ev.target.result;
  };
  reader.readAsDataURL(imgFile);
}

function handleTxtCoverFileSelect(fileList) {
  if (fileList && fileList.length > 0) processTxtCoverImageFile(fileList[0]);
}

function clearTxtCoverSelection() {
  txtCoverBmpB64 = '';
  const inp = document.getElementById('txtCoverInput');
  if (inp) inp.value = '';
  const clearBtn = document.getElementById('txtCoverClearBtn');
  if (clearBtn) clearBtn.style.display = 'none';
  const cap = document.getElementById('txtCoverPreviewCaption');
  if (cap) cap.textContent = 'Cloth-Spine Default';
  const lbl = document.getElementById('txtCoverDropLabel');
  if (lbl) lbl.innerHTML = '🖼 Drop cover image, click to browse, or press <strong>Ctrl+V</strong>';
  drawProceduralTxtCanvasPreview();
}

function resetTxtCoverDrawer() {
  const drawer = document.getElementById('txtCoverDrawer');
  if (drawer) drawer.style.display = 'none';
  txtDefaultStemTitle = '';
  txtDefaultStemAuthor = '';
  const tEl = document.getElementById('txtMetaTitle');
  const aEl = document.getElementById('txtMetaAuthor');
  if (tEl) tEl.value = '';
  if (aEl) aEl.value = '';
  clearTxtCoverSelection();
}

function updateTxtCoverDrawerForFiles(files) {
  const drawer = document.getElementById('txtCoverDrawer');
  if (!drawer) return;
  if (files && files.length === 1 && isTxtOrMdFilename(files[0].name)) {
    const parsed = parseStemTitleAuthor(files[0].name);
    txtDefaultStemTitle = parsed.title;
    txtDefaultStemAuthor = parsed.author;
    document.getElementById('txtMetaTitle').value = parsed.title;
    document.getElementById('txtMetaAuthor').value = parsed.author;
    drawer.style.display = 'block';
    renderTxtCoverProceduralIfEmpty();
  } else {
    resetTxtCoverDrawer();
  }
}

async function maybeBindUploadedTxtCoverMeta(uploadedFile, totalBatchCount) {
  if (totalBatchCount !== 1 || !isTxtOrMdFilename(uploadedFile.name)) return;
  const title = (document.getElementById('txtMetaTitle')?.value || '').trim();
  const author = (document.getElementById('txtMetaAuthor')?.value || '').trim();
  const metaChanged = (title && title !== txtDefaultStemTitle) || (author !== txtDefaultStemAuthor);
  if (!txtCoverBmpB64 && !metaChanged) return;
  const fullPath = (currentPath.endsWith('/') ? currentPath : currentPath + '/') + uploadedFile.name;
  try {
    await fetch('/api/txt-cover-meta', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        path: fullPath,
        title: title,
        author: author,
        coverBmpB64: txtCoverBmpB64
      })
    });
  } catch (err) {
    console.warn('[TXT Cover] Failed to bind cover/metadata:', err);
  }
}

(function initTxtCoverListeners() {
  const dz = document.getElementById('txtCoverDropZone');
  if (dz) {
    dz.addEventListener('dragover', (e) => { e.preventDefault(); e.stopPropagation(); });
    dz.addEventListener('drop', (e) => {
      e.preventDefault();
      e.stopPropagation();
      if (e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files.length > 0) {
        processTxtCoverImageFile(e.dataTransfer.files[0]);
      }
    });
  }
  document.addEventListener('paste', (e) => {
    const modal = document.getElementById('uploadModal');
    const drawer = document.getElementById('txtCoverDrawer');
    if (!modal || !modal.open || !drawer || drawer.style.display === 'none') return;
    const items = e.clipboardData && e.clipboardData.items;
    if (!items) return;
    for (let i = 0; i < items.length; i++) {
      if (items[i].type.startsWith('image/')) {
        const blob = items[i].getAsFile();
        if (blob) {
          e.preventDefault();
          processTxtCoverImageFile(blob);
          break;
        }
      }
    }
  });
})();
