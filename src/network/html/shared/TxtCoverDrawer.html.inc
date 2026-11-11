      <!-- TXT Book Card & Cover Art Drawer (auto-expands for .txt / .md uploads) -->
      <div id="txtCoverDrawer" style="display:none; margin: 12px 0; padding: 12px 14px; border: 1.5px solid var(--border-color, #cbd5e1); border-radius: 8px; background: var(--surface-alt, rgba(148,163,184,0.08));">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:10px; gap:8px; flex-wrap:wrap;">
          <div style="font-weight:600; font-size:0.95em;">📖 TXT Book Card &amp; Cover Art <span style="font-weight:400; opacity:0.75;">(Optional)</span></div>
          <span style="font-size:0.78em; padding:2px 8px; border-radius:999px; border:1px solid currentColor; opacity:0.8;">1-Bit X3 Atkinson · 4.5 KB</span>
        </div>
        <div style="display:flex; gap:14px; align-items:flex-start; flex-wrap:wrap;">
          <div style="flex:1 1 220px; min-width:200px; display:flex; flex-direction:column; gap:8px;">
            <div>
              <label for="txtMetaTitle" style="display:block; font-size:0.82em; font-weight:600; margin-bottom:3px;">Book Title</label>
              <input type="text" id="txtMetaTitle" placeholder="Auto-filled from filename" oninput="renderTxtCoverProceduralIfEmpty()" style="width:100%; box-sizing:border-box; padding:6px 8px; border:1px solid var(--border-color, #cbd5e1); border-radius:5px; font-size:0.9em;">
            </div>
            <div>
              <label for="txtMetaAuthor" style="display:block; font-size:0.82em; font-weight:600; margin-bottom:3px;">Author</label>
              <input type="text" id="txtMetaAuthor" placeholder="e.g. Liu Cixin / 余华" oninput="renderTxtCoverProceduralIfEmpty()" style="width:100%; box-sizing:border-box; padding:6px 8px; border:1px solid var(--border-color, #cbd5e1); border-radius:5px; font-size:0.9em;">
            </div>
            <div id="txtCoverDropZone" onclick="document.getElementById('txtCoverInput').click()" style="margin-top:2px; padding:10px; border:1.5px dashed var(--border-color, #94a3b8); border-radius:6px; text-align:center; cursor:pointer; font-size:0.84em;">
              <input type="file" id="txtCoverInput" accept="image/*" style="display:none;" onchange="handleTxtCoverFileSelect(this.files)">
              <div id="txtCoverDropLabel">🖼 Drop cover image, click to browse, or press <strong>Ctrl+V</strong></div>
              <div style="font-size:0.78em; opacity:0.7; margin-top:2px;">Stored cleanly in <code>/.crosspoint/covers/</code> (no clutter in /books)</div>
            </div>
            <button type="button" id="txtCoverClearBtn" onclick="clearTxtCoverSelection()" style="display:none; align-self:flex-start; padding:4px 10px; font-size:0.8em; border-radius:4px; border:1px solid var(--border-color, #94a3b8); background:transparent; cursor:pointer;">Use Default Cloth-Spine Cover</button>
          </div>
          <div style="flex:0 0 auto; display:flex; flex-direction:column; align-items:center; gap:4px;">
            <canvas id="txtCoverPreviewCanvas" width="148" height="222" style="width:98px; height:147px; border:2px solid #000; background:#fff; image-rendering:pixelated;"></canvas>
            <span id="txtCoverPreviewCaption" style="font-size:0.75em; opacity:0.75;">Cloth-Spine Default</span>
          </div>
        </div>
      </div>
