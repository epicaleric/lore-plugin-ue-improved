export function timeAgo(raw: string): string {
  const when = Date.parse(raw);
  if (Number.isNaN(when)) return raw;
  const seconds = Math.max(0, (Date.now() - when) / 1000);
  if (seconds < 60) return "just now";
  const minutes = seconds / 60;
  if (minutes < 60) return `${Math.floor(minutes)} min ago`;
  const hours = minutes / 60;
  if (hours < 24) return `${Math.floor(hours)} h ago`;
  const days = hours / 24;
  if (days < 30) return `${Math.floor(days)} d ago`;
  return new Date(when).toLocaleDateString(undefined, { day: "numeric", month: "short", year: "numeric" });
}

export function fullDate(raw: string): string {
  const when = Date.parse(raw);
  if (Number.isNaN(when)) return raw;
  return new Date(when).toLocaleString(undefined, { day: "numeric", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit" });
}

function hash(text: string): number {
  let value = 0;
  for (let i = 0; i < text.length; i++) {
    value = (value * 31 + text.charCodeAt(i)) >>> 0;
  }
  return value;
}

export function hueFor(text: string): number {
  return hash(text.toLowerCase()) % 360;
}

export function avatarColor(name: string): string {
  return `hsl(${hueFor(name)} 55% 52%)`;
}

export function placeholderGradient(name: string): string {
  const hue = hueFor(name);
  return `linear-gradient(135deg, hsl(${hue} 45% 22%), hsl(${(hue + 50) % 360} 50% 14%))`;
}

export function initials(name: string): string {
  const parts = name.replace(/[_-]+/g, " ").split(/\s+|(?=[A-Z])/).filter(Boolean);
  return (parts[0]?.[0] ?? "?").toUpperCase() + (parts[1]?.[0] ?? "").toUpperCase();
}

export function hostOf(url: string): string {
  return url.replace(/^[a-z]+:\/\//i, "").split(/[:/]/)[0];
}

export function uid(): string {
  return Math.random().toString(36).slice(2, 10);
}

/** Readable text colour for a given CSS hex background. */
export function readableOn(hex: string): string {
  const value = hex.replace("#", "");
  const r = parseInt(value.slice(0, 2), 16) / 255;
  const g = parseInt(value.slice(2, 4), 16) / 255;
  const b = parseInt(value.slice(4, 6), 16) / 255;
  const lin = (c: number) => (c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4));
  const luminance = 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
  return luminance > 0.33 ? "#0b0d12" : "#ffffff";
}

export function formatBytes(bytes: number): string {
  if (bytes >= 1024 ** 3) return `${(bytes / 1024 ** 3).toFixed(1)} GB`;
  if (bytes >= 1024 ** 2) return `${(bytes / 1024 ** 2).toFixed(bytes >= 100 * 1024 ** 2 ? 0 : 1)} MB`;
  if (bytes >= 1024) return `${Math.round(bytes / 1024)} KB`;
  return `${bytes} B`;
}
