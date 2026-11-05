import { useEffect, useRef, useState, type CSSProperties, type ReactNode } from "react";
import { Loader2 } from "lucide-react";
import { avatarColor, initials, placeholderGradient } from "../lib/format";
import { api } from "../lib/api";

export function Spinner({ size = 16 }: { size?: number }) {
  return <Loader2 size={size} className="spin" />;
}

export function Avatar({ name, size }: { name: string; size?: "small" | "large" }) {
  return (
    <span className={`avatar ${size ?? ""}`} style={{ background: avatarColor(name) }} title={name}>
      {(name || "?").slice(0, 1).toUpperCase()}
    </span>
  );
}

export function Pill({ tone = "flat", children, title }: { tone?: "good" | "warn" | "bad" | "info" | "muted" | "accent" | "flat"; children: ReactNode; title?: string }) {
  return (
    <span className={`pill ${tone === "flat" ? "flat" : tone}`} title={title}>
      {children}
    </span>
  );
}

export function Switch({ on, onChange }: { on: boolean; onChange: (on: boolean) => void }) {
  return <button className={`switch ${on ? "on" : ""}`} role="switch" aria-checked={on} onClick={() => onChange(!on)} />;
}

export function Field({ label, hint, children }: { label: string; hint?: string; children: ReactNode }) {
  return (
    <div className="field">
      <label>{label}</label>
      {children}
      {hint && <span className="hint">{hint}</span>}
    </div>
  );
}

export function Modal({ title, sub, onClose, children, actions, wide }: { title: string; sub?: string; onClose: () => void; children: ReactNode; actions?: ReactNode; wide?: boolean }) {
  useEffect(() => {
    const handler = (event: KeyboardEvent) => event.key === "Escape" && onClose();
    window.addEventListener("keydown", handler);
    return () => window.removeEventListener("keydown", handler);
  }, [onClose]);
  return (
    <div className="scrim" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div className={`modal ${wide ? "wide" : ""}`}>
        <h2>{title}</h2>
        {sub && <div className="sub">{sub}</div>}
        {children}
        {actions && <div className="actions">{actions}</div>}
      </div>
    </div>
  );
}

export interface MenuItem {
  label: string;
  icon?: ReactNode;
  onClick: () => void;
  danger?: boolean;
  divider?: boolean;
}

export function Menu({ x, y, items, onClose }: { x: number; y: number; items: MenuItem[]; onClose: () => void }) {
  const ref = useRef<HTMLDivElement>(null);
  const [pos, setPos] = useState({ x, y });
  useEffect(() => {
    const rect = ref.current?.getBoundingClientRect();
    if (rect) {
      setPos({ x: Math.min(x, window.innerWidth - rect.width - 12), y: Math.min(y, window.innerHeight - rect.height - 12) });
    }
    const close = () => onClose();
    window.addEventListener("mousedown", close);
    window.addEventListener("blur", close);
    return () => {
      window.removeEventListener("mousedown", close);
      window.removeEventListener("blur", close);
    };
  }, []);
  return (
    <div ref={ref} className="menu" style={{ left: pos.x, top: pos.y }} onMouseDown={(e) => e.stopPropagation()}>
      {items.map((item, index) =>
        item.divider ? (
          <hr key={index} />
        ) : (
          <button
            key={index}
            style={item.danger ? { color: "var(--bad)" } : undefined}
            onClick={() => {
              onClose();
              item.onClick();
            }}
          >
            {item.icon}
            {item.label}
          </button>
        ),
      )}
    </div>
  );
}

export function useMenu() {
  const [menu, setMenu] = useState<{ x: number; y: number; items: MenuItem[] } | null>(null);
  return {
    open: (event: React.MouseEvent, items: MenuItem[]) => {
      event.preventDefault();
      event.stopPropagation();
      setMenu({ x: event.clientX, y: event.clientY, items });
    },
    element: menu ? <Menu x={menu.x} y={menu.y} items={menu.items} onClose={() => setMenu(null)} /> : null,
  };
}

const imageCache = new Map<string, string | null>();

export function useImage(path: string | null | undefined) {
  const [src, setSrc] = useState<string | null>(path ? imageCache.get(path) ?? null : null);
  useEffect(() => {
    if (!path) {
      setSrc(null);
      return;
    }
    if (imageCache.has(path)) {
      setSrc(imageCache.get(path) ?? null);
      return;
    }
    let cancelled = false;
    api.readImage(path).then((data) => {
      imageCache.set(path, data);
      if (!cancelled) setSrc(data);
    });
    return () => {
      cancelled = true;
    };
  }, [path]);
  return src;
}

export function Thumb({ name, image, style, children, className }: { name: string; image: string | null | undefined; style?: CSSProperties; children?: ReactNode; className?: string }) {
  const src = useImage(image);
  return (
    <div className={className ?? "thumb"} style={{ background: src ? `center / cover url(${src})` : placeholderGradient(name), ...style }}>
      {!src && <span className="initials">{initials(name)}</span>}
      {children}
    </div>
  );
}

export function Empty({ icon, title, children }: { icon: ReactNode; title: string; children?: ReactNode }) {
  return (
    <div className="empty">
      <div className="big">{icon}</div>
      <h2>{title}</h2>
      <div>{children}</div>
    </div>
  );
}
