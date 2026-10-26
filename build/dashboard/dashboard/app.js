// Advanced Distributed File System - Interactive Frontend Dashboard
const API_BASE = '';

// DOM Elements & State
let activeTab = 'nodes';
let currentFiles = [];

// Initialize
document.addEventListener('DOMContentLoaded', () => {
  setupTabs();
  setupEventListeners();
  fetchInitialData();
  startPolling();
});

// Tab Navigation
function setupTabs() {
  const tabButtons = document.querySelectorAll('.tab-btn');
  tabButtons.forEach(btn => {
    btn.addEventListener('click', () => {
      tabButtons.forEach(b => b.classList.remove('active'));
      document.querySelectorAll('.tab-pane').forEach(p => p.classList.remove('active'));

      btn.classList.add('active');
      const tabId = btn.getAttribute('data-tab');
      const targetPane = document.getElementById(`tab-${tabId}`);
      if (targetPane) targetPane.classList.add('active');
      activeTab = tabId;

      if (activeTab === 'disksim') loadDiskSimulation();
    });
  });
}

function setupEventListeners() {
  // Upload modal controls
  const openUploadBtn = document.getElementById('openUploadModalBtn');
  const closeUploadBtn = document.getElementById('closeUploadBtn');
  const uploadCard = document.getElementById('uploadCard');
  const uploadForm = document.getElementById('uploadForm');

  if (openUploadBtn) {
    openUploadBtn.addEventListener('click', () => {
      uploadCard.style.display = 'block';
    });
  }
  if (closeUploadBtn) {
    closeUploadBtn.addEventListener('click', () => {
      uploadCard.style.display = 'none';
    });
  }
  if (uploadForm) {
    uploadForm.addEventListener('submit', handleUpload);
  }

  // Refresh files
  const refreshFilesBtn = document.getElementById('refreshFilesBtn');
  if (refreshFilesBtn) refreshFilesBtn.addEventListener('click', loadFiles);

  // File search filter
  const fileSearchInput = document.getElementById('fileSearchInput');
  if (fileSearchInput) {
    fileSearchInput.addEventListener('input', (e) => {
      renderFiles(e.target.value.toLowerCase());
    });
  }

  // Close versions
  const closeVersionBtn = document.getElementById('closeVersionBtn');
  if (closeVersionBtn) {
    closeVersionBtn.addEventListener('click', () => {
      document.getElementById('versionCard').style.display = 'none';
    });
  }

  // Scheduler set
  const setAlgoBtn = document.getElementById('setAlgoBtn');
  if (setAlgoBtn) {
    setAlgoBtn.addEventListener('click', async () => {
      const algo = document.getElementById('algoSelect').value;
      try {
        await fetch(`${API_BASE}/api/scheduler/set?algo=${algo}`, { method: 'POST' });
        loadScheduler();
      } catch (err) {
        console.error('Error setting algorithm:', err);
      }
    });
  }

  // Clear cache
  const clearCacheBtn = document.getElementById('clearCacheBtn');
  if (clearCacheBtn) {
    clearCacheBtn.addEventListener('click', async () => {
      await fetch(`${API_BASE}/api/cache/clear`, { method: 'POST' });
      loadCache();
    });
  }

  // Trigger recovery
  const runRecoveryBtn = document.getElementById('runRecoveryBtn');
  if (runRecoveryBtn) {
    runRecoveryBtn.addEventListener('click', async () => {
      try {
        const resp = await fetch(`${API_BASE}/api/recovery/run`, { method: 'POST' });
        const res = await resp.json();
        alert(`Recovery Scan Completed:\n- Uncommitted Found: ${res.pending_operations_found}\n- Rolled Back: ${res.operations_rolled_back}`);
        loadRecovery();
      } catch (err) {
        console.error('Error triggering recovery:', err);
      }
    });
  }

  // Disk Sim Form
  const diskSimForm = document.getElementById('diskSimForm');
  if (diskSimForm) {
    diskSimForm.addEventListener('submit', handleDiskSim);
  }
}

// Live Polling Cycle
function startPolling() {
  setInterval(async () => {
    try {
      await loadStatus();
      await loadNodes();

      if (activeTab === 'files') await loadFiles();
      else if (activeTab === 'scheduler') await loadScheduler();
      else if (activeTab === 'buffer') { await loadBuffer(); await loadCache(); }
      else if (activeTab === 'sync') await loadSync();
      else if (activeTab === 'recovery') await loadRecovery();
      else if (activeTab === 'audit') await loadAudit();
    } catch (e) {
      console.warn('Polling error:', e);
    }
  }, 2000);
}

function fetchInitialData() {
  loadStatus();
  loadNodes();
  loadFiles();
  loadScheduler();
  loadBuffer();
  loadCache();
  loadSync();
  loadRecovery();
  loadAudit();
}

// 1. Status & Top Ribbon
async function loadStatus() {
  try {
    const res = await fetch(`${API_BASE}/api/status`);
    if (!res.ok) return;
    const data = await res.json();

    document.getElementById('coordPid').textContent = data.coordinator_pid || '--';
    document.getElementById('activeThreads').textContent = data.active_worker_threads || '0';
  } catch (err) {}
}

// 2. Storage Nodes & Fault Injection
async function loadNodes() {
  try {
    const res = await fetch(`${API_BASE}/api/nodes`);
    if (!res.ok) return;
    const nodes = await res.json();

    let onlineCount = 0;
    const container = document.getElementById('nodesContainer');
    let html = '';
    let processHtml = '';

    nodes.forEach(node => {
      const isOnline = node.status === 'ONLINE' || node.is_process_running;
      if (isOnline) onlineCount++;

      const statusBadge = isOnline
        ? '<span class="status-badge badge-green">ONLINE</span>'
        : '<span class="status-badge badge-red">OFFLINE</span>';

      const usedMB = (node.used_space / (1024 * 1024)).toFixed(2);
      const totalGB = (node.total_space / (1024 * 1024 * 1024)).toFixed(1);
      const pct = node.total_space > 0 ? ((node.used_space / node.total_space) * 100).toFixed(1) : 0;

      html += `
        <div class="node-card ${isOnline ? 'online' : 'offline'}">
          <div class="node-header">
            <span class="node-title">${node.name}</span>
            ${statusBadge}
          </div>
          <div class="node-meta">
            <span>Port: ${node.port}</span>
            <span>PID: ${node.process_id || '--'}</span>
          </div>
          <div class="buffer-gauge-wrapper">
            <div class="gauge-title">Storage Used: ${usedMB} MB / ${totalGB} GB (${pct}%)</div>
            <div class="progress-bar-bg">
              <div class="progress-bar-fill" style="width: ${Math.max(pct, 2)}%;"></div>
            </div>
          </div>
          <div class="node-actions">
            ${isOnline 
              ? `<button class="btn btn-sm btn-danger" onclick="killNode(${node.id})">Simulate Crash (Kill)</button>` 
              : `<button class="btn btn-sm btn-success" onclick="startNode(${node.id})">Revive Node (Start)</button>`}
          </div>
        </div>
      `;

      // Also update native process table
      const memMB = (node.memory_working_set_bytes ? (node.memory_working_set_bytes / (1024 * 1024)).toFixed(1) : 0);
      processHtml += `
        <tr>
          <td><strong>${node.name}</strong></td>
          <td><code>${node.process_id || 'N/A'}</code></td>
          <td>Storage Node Process</td>
          <td>${memMB > 0 ? memMB + ' MB' : 'Active'}</td>
          <td>${node.port}</td>
          <td>${statusBadge}</td>
        </tr>
      `;
    });

    document.getElementById('kpiNodes').textContent = `${onlineCount} / ${nodes.length}`;
    if (container) container.innerHTML = html;

    const procBody = document.getElementById('processTableBody');
    if (procBody) {
      const coordPid = document.getElementById('coordPid').textContent;
      procBody.innerHTML = `
        <tr>
          <td><strong>dfs_coordinator.exe</strong></td>
          <td><code>${coordPid}</code></td>
          <td>Central Coordinator Server</td>
          <td>Managed Working Set</td>
          <td>9001 (RPC) / 8080 (Web)</td>
          <td><span class="status-badge badge-green">RUNNING</span></td>
        </tr>
        ${processHtml}
      `;
    }
  } catch (err) {
    console.error('Error loading nodes:', err);
  }
}

async function killNode(id) {
  try {
    await fetch(`${API_BASE}/api/nodes/kill`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ node_id: id })
    });
    loadNodes();
  } catch (e) {
    alert('Error killing node: ' + e);
  }
}

async function startNode(id) {
  try {
    await fetch(`${API_BASE}/api/nodes/start`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ node_id: id })
    });
    loadNodes();
  } catch (e) {
    alert('Error starting node: ' + e);
  }
}

// 3. File Explorer
async function loadFiles() {
  try {
    const res = await fetch(`${API_BASE}/api/files`);
    if (!res.ok) return;
    currentFiles = await res.json();
    document.getElementById('kpiFiles').textContent = currentFiles.length;
    renderFiles('');
  } catch (err) {
    console.error('Error loading files:', err);
  }
}

function renderFiles(filter) {
  const tbody = document.getElementById('filesTableBody');
  if (!tbody) return;

  const filtered = currentFiles.filter(f => f.path.toLowerCase().includes(filter));
  if (filtered.length === 0) {
    tbody.innerHTML = '<tr><td colspan="7" class="text-center">No files found</td></tr>';
    return;
  }

  let html = '';
  filtered.forEach(f => {
    let replicaTags = '';
    if (f.chunks && f.chunks.length > 0) {
      const nodeIds = [...new Set(f.chunks.flatMap(c => c.replicas || []))];
      replicaTags = nodeIds.map(nid => `<span class="status-badge badge-green">Node ${nid}</span>`).join(' ');
    } else {
      replicaTags = '<span class="status-badge badge-yellow">No chunks</span>';
    }

    const shortHash = f.checksum_sha256 ? f.checksum_sha256.substring(0, 12) + '...' : '--';

    html += `
      <tr>
        <td><strong>${escapeHtml(f.path)}</strong></td>
        <td>${formatBytes(f.size)}</td>
        <td>${escapeHtml(f.owner)}</td>
        <td>v${f.version}</td>
        <td><code title="${f.checksum_sha256}">${shortHash}</code></td>
        <td>${replicaTags}</td>
        <td>
          <button class="btn btn-sm btn-primary" onclick="downloadFile('${escapeAttr(f.path)}')">Download</button>
          <button class="btn btn-sm btn-secondary" onclick="viewVersions('${escapeAttr(f.path)}')">History</button>
          <button class="btn btn-sm btn-secondary" onclick="promptRename('${escapeAttr(f.path)}')">Rename</button>
          <button class="btn btn-sm btn-danger" onclick="deleteFile('${escapeAttr(f.path)}')">Delete</button>
        </td>
      </tr>
    `;
  });
  tbody.innerHTML = html;
}

async function handleUpload(e) {
  e.preventDefault();
  const path = document.getElementById('uploadPath').value;
  const content = document.getElementById('uploadContent').value;
  const repl = parseInt(document.getElementById('uploadRepl').value);
  const owner = document.getElementById('uploadUser').value;

  try {
    const res = await fetch(`${API_BASE}/api/files/upload`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        path: path,
        content: content,
        replication_factor: repl,
        owner: owner
      })
    });
    const result = await res.json();
    if (res.ok) {
      document.getElementById('uploadCard').style.display = 'none';
      document.getElementById('uploadForm').reset();
      loadFiles();
      loadBuffer();
      loadAudit();
    } else {
      alert('Upload failed: ' + (result.error || 'Unknown error'));
    }
  } catch (err) {
    alert('Upload error: ' + err);
  }
}

function downloadFile(path) {
  window.open(`${API_BASE}/api/files/download?path=${encodeURIComponent(path)}`, '_blank');
}

async function deleteFile(path) {
  if (!confirm(`Are you sure you want to delete ${path}?`)) return;
  try {
    await fetch(`${API_BASE}/api/files?path=${encodeURIComponent(path)}`, { method: 'DELETE' });
    loadFiles();
    loadAudit();
  } catch (e) {
    alert('Delete error: ' + e);
  }
}

async function promptRename(oldPath) {
  const newPath = prompt(`Enter new path for ${oldPath}:`, oldPath);
  if (!newPath || newPath === oldPath) return;

  try {
    const res = await fetch(`${API_BASE}/api/files/rename`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ old_path: oldPath, new_path: newPath })
    });
    if (res.ok) {
      loadFiles();
      loadAudit();
    } else {
      alert('Rename failed');
    }
  } catch (e) {
    alert('Rename error: ' + e);
  }
}

async function viewVersions(path) {
  try {
    const res = await fetch(`${API_BASE}/api/files/versions?path=${encodeURIComponent(path)}`);
    const versions = await res.json();

    document.getElementById('versionPathTitle').textContent = path;
    const tbody = document.getElementById('versionTableBody');

    if (versions.length === 0) {
      tbody.innerHTML = '<tr><td colspan="6" class="text-center">No previous version snapshots found</td></tr>';
    } else {
      let html = '';
      versions.forEach(v => {
        const dateStr = new Date(v.created_at * 1000).toLocaleString();
        html += `
          <tr>
            <td><strong>v${v.version}</strong></td>
            <td>${formatBytes(v.size)}</td>
            <td><code>${v.checksum.substring(0, 12)}...</code></td>
            <td>${dateStr}</td>
            <td>${escapeHtml(v.created_by)}</td>
            <td>
              <button class="btn btn-sm btn-primary" onclick="restoreVersion('${escapeAttr(path)}', ${v.version})">Restore v${v.version}</button>
            </td>
          </tr>
        `;
      });
      tbody.innerHTML = html;
    }
    document.getElementById('versionCard').style.display = 'block';
  } catch (e) {
    console.error('Error fetching versions:', e);
  }
}

async function restoreVersion(path, version) {
  if (!confirm(`Restore ${path} to version ${version}?`)) return;
  try {
    const res = await fetch(`${API_BASE}/api/files/restore`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path: path, version: version })
    });
    if (res.ok) {
      alert('Restored successfully');
      loadFiles();
      document.getElementById('versionCard').style.display = 'none';
    }
  } catch (e) {
    alert('Restore error: ' + e);
  }
}

// 4. Request Scheduler
async function loadScheduler() {
  try {
    const res = await fetch(`${API_BASE}/api/scheduler`);
    if (!res.ok) return;
    const data = await res.json();

    document.getElementById('kpiScheduler').textContent = data.current_algorithm;
    document.getElementById('kpiThroughput').textContent = `${data.throughput_ops_sec.toFixed(2)} ops/sec`;

    document.getElementById('queueDepth').textContent = data.queue_depth;
    document.getElementById('metricWaitTime').textContent = `${data.avg_waiting_time_ms.toFixed(2)} ms`;
    document.getElementById('metricRespTime').textContent = `${data.avg_response_time_ms.toFixed(2)} ms`;
    document.getElementById('metricTurnaround').textContent = `${data.avg_turnaround_time_ms.toFixed(2)} ms`;
    document.getElementById('metricThroughput').textContent = `${data.throughput_ops_sec.toFixed(2)} ops/sec`;

    const qElem = document.getElementById('queueVisualizer');
    if (!data.queue || data.queue.length === 0) {
      qElem.innerHTML = '<div class="empty-state">No pending requests in scheduler queue (idle)</div>';
    } else {
      let qHtml = '';
      data.queue.forEach(item => {
        qHtml += `
          <div class="arch-step mb-2" style="padding: 0.6rem 1rem;">
            <strong>[${item.operation}]</strong> ${escapeHtml(item.path)} 
            <span class="status-badge badge-yellow">Priority: ${item.priority}</span>
            <span class="text-muted">Client: ${escapeHtml(item.client)}</span>
          </div>
        `;
      });
      qElem.innerHTML = qHtml;
    }
  } catch (err) {
    console.error('Error loading scheduler:', err);
  }
}

// 5. Memory Buffer & Cache
async function loadBuffer() {
  try {
    const res = await fetch(`${API_BASE}/api/buffer`);
    if (!res.ok) return;
    const b = await res.json();

    const pct = b.capacity > 0 ? ((b.current_occupancy / b.capacity) * 100).toFixed(0) : 0;
    document.getElementById('kpiBuffer').textContent = `${pct}%`;
    document.getElementById('kpiBlocked').textContent = `${b.blocked_producers} Blocked Producers`;

    document.getElementById('bufOccupancyText').textContent = `${b.current_occupancy} / ${b.capacity} Slots`;
    document.getElementById('bufFillBar').style.width = `${pct}%`;
    document.getElementById('bufProduced').textContent = b.total_produced;
    document.getElementById('bufConsumed').textContent = b.total_consumed;
    document.getElementById('bufBlockedProd').textContent = b.blocked_producers;
    document.getElementById('bufWatermark').textContent = `${b.high_watermark_pct.toFixed(1)}%`;
  } catch (e) {}
}

async function loadCache() {
  try {
    const res = await fetch(`${API_BASE}/api/cache`);
    if (!res.ok) return;
    const c = await res.json();

    document.getElementById('kpiCache').textContent = `${c.hit_ratio_pct.toFixed(1)}%`;
    document.getElementById('kpiCacheHits').textContent = `${c.hit_count} Hits / ${c.miss_count} Misses`;

    document.getElementById('cacheRatio').textContent = `${c.hit_ratio_pct.toFixed(1)}%`;
    document.getElementById('cacheCap').textContent = `${c.current_items} / ${c.capacity_items} items`;
    document.getElementById('cacheEvict').textContent = c.eviction_count;
    document.getElementById('cacheInval').textContent = c.invalidation_count;

    const keyList = document.getElementById('cachedKeysList');
    if (!c.cached_keys || c.cached_keys.length === 0) {
      keyList.innerHTML = '<span class="text-muted">Cache is currently empty</span>';
    } else {
      keyList.innerHTML = c.cached_keys.map(k => `<div>&bull; ${escapeHtml(k)}</div>`).join('');
    }
  } catch (e) {}
}

// 6. Synchronization & Deadlocks
async function loadSync() {
  try {
    const res = await fetch(`${API_BASE}/api/locks`);
    if (!res.ok) return;
    const data = await res.json();

    document.getElementById('syncActiveCount').textContent = data.active_lock_count;
    document.getElementById('syncWaitCount').textContent = data.waiting_request_count;
    document.getElementById('syncTimeouts').textContent = data.total_timeouts;
    document.getElementById('syncPrevented').textContent = data.total_deadlocks_prevented;

    const banner = document.getElementById('deadlockBanner');
    if (data.has_deadlock_cycle) {
      banner.className = 'deadlock-alert';
      banner.style.background = 'rgba(244, 63, 94, 0.2)';
      banner.style.borderColor = 'rgba(244, 63, 94, 0.5)';
      banner.innerHTML = `<span class="status-badge badge-red">DEADLOCK DETECTED</span> Wait-For Cycle: ${data.deadlock_cycle.join(' &rarr; ')}`;
    } else {
      banner.className = 'deadlock-alert';
      banner.style.background = 'rgba(16, 185, 129, 0.1)';
      banner.style.borderColor = 'rgba(16, 185, 129, 0.3)';
      banner.innerHTML = `<span class="status-badge badge-green">SAFE</span> No Deadlock Cycles Detected (Lock Hierarchy Order Enforced)`;
    }

    const locksBody = document.getElementById('activeLocksBody');
    if (!data.active_locks || data.active_locks.length === 0) {
      locksBody.innerHTML = '<tr><td colspan="4" class="text-center">No active locks held</td></tr>';
    } else {
      let html = '';
      data.active_locks.forEach(l => {
        html += `
          <tr>
            <td><strong>${escapeHtml(l.resource)}</strong></td>
            <td><span class="status-badge ${l.type === 'SHARED_READ' ? 'badge-yellow' : 'badge-red'}">${l.type}</span></td>
            <td>${escapeHtml(l.client)}</td>
            <td>${new Date(l.acquired_at * 1000).toLocaleTimeString()}</td>
          </tr>
        `;
      });
      locksBody.innerHTML = html;
    }
  } catch (e) {}
}

// 7. Crash Recovery & WAL
async function loadRecovery() {
  try {
    const res = await fetch(`${API_BASE}/api/recovery`);
    if (!res.ok) return;
    const logs = await res.json();

    const tbody = document.getElementById('walTableBody');
    if (!logs || logs.length === 0) {
      tbody.innerHTML = '<tr><td colspan="6" class="text-center">No WAL entries found</td></tr>';
      return;
    }

    let html = '';
    logs.forEach(w => {
      const stateBadge = w.state === 'COMMITTED'
        ? '<span class="status-badge badge-green">COMMITTED</span>'
        : (w.state === 'PENDING' ? '<span class="status-badge badge-yellow">PENDING</span>' : '<span class="status-badge badge-red">ROLLED_BACK</span>');

      html += `
        <tr>
          <td><code>${w.lsn}</code></td>
          <td><code>${escapeHtml(w.tx_id)}</code></td>
          <td><strong>${escapeHtml(w.operation)}</strong></td>
          <td>${escapeHtml(w.file_path)}</td>
          <td>${stateBadge}</td>
          <td>${new Date(w.timestamp * 1000).toLocaleTimeString()}</td>
        </tr>
      `;
    });
    tbody.innerHTML = html;
  } catch (e) {}
}

// 8. Disk Seek Simulator
async function loadDiskSimulation() {
  try {
    const res = await fetch(`${API_BASE}/api/disk_sim`);
    if (!res.ok) return;
    const data = await res.json();
    renderDiskResults(data);
  } catch (e) {}
}

async function handleDiskSim(e) {
  e.preventDefault();
  const start = parseInt(document.getElementById('diskStartCyl').value);
  const reqStr = document.getElementById('diskRequests').value;
  const requests = reqStr.split(',').map(s => parseInt(s.trim())).filter(n => !isNaN(n));

  try {
    const res = await fetch(`${API_BASE}/api/disk_sim/run`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ start_cylinder: start, requests: requests, max_cylinder: 199 })
    });
    const data = await res.json();
    renderDiskResults(data);
  } catch (err) {
    alert('Sim error: ' + err);
  }
}

function renderDiskResults(data) {
  const container = document.getElementById('diskResultsGrid');
  if (!container) return;

  const algos = [
    { title: 'FCFS (First-Come First-Served)', data: data.fcfs, color: 'text-amber' },
    { title: 'SSTF (Shortest Seek Time First)', data: data.sstf, color: 'text-emerald' },
    { title: 'SCAN (Elevator Algorithm)', data: data.scan, color: 'text-cyan' }
  ];

  let html = '';
  algos.forEach(a => {
    const isBest = a.data.algorithm === data.best_algorithm;
    html += `
      <div class="sim-card" style="${isBest ? 'border: 1px solid var(--accent-emerald); box-shadow: 0 0 15px rgba(16, 185, 129, 0.2);' : ''}">
        <div class="sim-algo-title">
          <span>${a.title}</span>
          ${isBest ? '<span class="status-badge badge-green">OPTIMAL</span>' : ''}
        </div>
        <div class="metrics-grid mt-2">
          <div class="metric-box">
            <span class="m-label">Total Head Movement:</span>
            <span class="m-val ${a.color}">${a.data.total_head_movement} cyl</span>
          </div>
          <div class="metric-box">
            <span class="m-label">Average Seek:</span>
            <span class="m-val">${a.data.avg_seek_distance.toFixed(1)} cyl</span>
          </div>
        </div>
        <div class="sim-trajectory">
          <strong>Seek Sequence:</strong><br>
          ${a.data.seek_sequence.join(' &rarr; ')}
        </div>
      </div>
    `;
  });
  container.innerHTML = html;
}

// 9. Audit Trail
async function loadAudit() {
  try {
    const res = await fetch(`${API_BASE}/api/audit`);
    if (!res.ok) return;
    const logs = await res.json();

    const tbody = document.getElementById('auditTableBody');
    if (!logs || logs.length === 0) {
      tbody.innerHTML = '<tr><td colspan="8" class="text-center">No audit logs recorded yet</td></tr>';
      return;
    }

    let html = '';
    logs.forEach(a => {
      const resBadge = a.success
        ? '<span class="status-badge badge-green">SUCCESS</span>'
        : '<span class="status-badge badge-red">DENIED</span>';

      html += `
        <tr>
          <td>#${a.id}</td>
          <td>${new Date(a.timestamp * 1000).toLocaleTimeString()}</td>
          <td><strong>${escapeHtml(a.user)}</strong></td>
          <td><code>${escapeHtml(a.action)}</code></td>
          <td>${escapeHtml(a.target_path)}</td>
          <td>${resBadge}</td>
          <td>${escapeHtml(a.client_ip)}</td>
          <td><small>${escapeHtml(a.details)}</small></td>
        </tr>
      `;
    });
    tbody.innerHTML = html;
  } catch (e) {}
}

// Helper formatting functions
function formatBytes(bytes) {
  if (bytes === 0) return '0 B';
  const k = 1024;
  const sizes = ['B', 'KB', 'MB', 'GB'];
  const i = Math.floor(Math.log(bytes) / Math.log(k));
  return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

function escapeHtml(str) {
  if (!str) return '';
  return str.replace(/[&<>"']/g, m => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  }[m]));
}

function escapeAttr(str) {
  if (!str) return '';
  return str.replace(/"/g, '&quot;');
}
