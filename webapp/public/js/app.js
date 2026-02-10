/* ============================================
   IoT Dashboard - Shared Frontend JS
   Socket.IO + Utility functions
   ============================================ */

// Socket.IO connection
const socket = io();

// ============================================
// MQTT STATUS (shared across all pages)
// ============================================
socket.on('mqtt:status', (data) => {
  const dot = document.getElementById('mqtt-dot');
  const text = document.getElementById('mqtt-text');
  if (!dot || !text) return;

  if (data.connected) {
    dot.className = 'mqtt-status-dot connected';
    text.textContent = 'MQTT Connecté';
  } else {
    dot.className = 'mqtt-status-dot disconnected';
    text.textContent = 'MQTT Déconnecté';
  }
});

socket.on('connect', () => {
  console.log('🔌 Socket.IO connected');
});

socket.on('disconnect', () => {
  console.log('🔌 Socket.IO disconnected');
  const dot = document.getElementById('mqtt-dot');
  const text = document.getElementById('mqtt-text');
  if (dot) dot.className = 'mqtt-status-dot disconnected';
  if (text) text.textContent = 'Serveur déconnecté';
});

// ============================================
// UTILITY: Time since
// ============================================
function timeSince(timestamp) {
  const seconds = Math.floor((Date.now() - timestamp) / 1000);

  if (seconds < 5) return 'À l\'instant';
  if (seconds < 60) return `il y a ${seconds}s`;
  
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return `il y a ${minutes}min`;
  
  const hours = Math.floor(minutes / 60);
  if (hours < 24) return `il y a ${hours}h`;
  
  const days = Math.floor(hours / 24);
  return `il y a ${days}j`;
}

// ============================================
// UTILITY: Toast notifications
// ============================================
function showToast(message, type = 'info') {
  const container = document.getElementById('toast-container');
  if (!container) return;

  const toast = document.createElement('div');
  toast.className = `toast ${type}`;
  toast.innerHTML = `<span>${message}</span>`;
  container.appendChild(toast);

  setTimeout(() => {
    toast.style.opacity = '0';
    toast.style.transform = 'translateX(100%)';
    toast.style.transition = 'all 0.3s ease';
    setTimeout(() => toast.remove(), 300);
  }, 4000);
}
