// Icon outlines on a 24 × 24 grid. `solid` icons are filled, the others stroked.

export interface Icon {
  path: string
  solid?: true
}

const table = {
  play: { path: 'M7 4.5v15l12.5-7.5z', solid: true },
  pause: { path: 'M6.5 5h4v14h-4zM13.5 5h4v14h-4z', solid: true },
  stop: { path: 'M6 6h12v12H6z', solid: true },
  step: { path: 'M5 4.5v15l9.5-7.5zM16 5h3v14h-3z', solid: true },
  home: { path: 'M4 11.5 12 4l8 7.5M6.5 10v9.5h11V10' },
  folder: { path: 'M3.5 6.5h6l2 2.5h9v10.5h-17z' },
  file: { path: 'M6.5 3.5h7l4 4v13h-11zM13.5 3.5v4h4' },
  up: { path: 'M6 15l6-6 6 6' },
  down: { path: 'M6 9l6 6 6-6' },
  left: { path: 'M15 6l-6 6 6 6' },
  right: { path: 'M9 6l6 6-6 6' },
  close: { path: 'M6 6l12 12M18 6 6 18' },
  plus: { path: 'M12 5v14M5 12h14' },
  minus: { path: 'M5 12h14' },
  trash: { path: 'M5 7h14M9.5 7V4.5h5V7M7 7l1 12.5h8L17 7' },
  warning: { path: 'M12 4 2.5 20h19zM12 10v5M12 17.4v.2' },
  info: { path: 'M12 3.5a8.5 8.5 0 1 0 0 17 8.5 8.5 0 1 0 0-17M12 11v6M12 7.4v.2' },
  check: { path: 'M5 12.5l4.5 4.5L19 7.5' },
  power: { path: 'M12 3.5v8M7 6.5a7 7 0 1 0 10 0' },
  forward: { path: 'M19 12a7 7 0 1 1-2.5-5.4M19 4.5V9h-4.5' },
  reverse: { path: 'M5 12a7 7 0 1 0 2.5-5.4M5 4.5V9h4.5' },
  drop: { path: 'M12 3.5c3 4 5.5 6.8 5.5 10a5.5 5.5 0 0 1-11 0c0-3.2 2.5-6 5.5-10z' },
  list: { path: 'M5 7h14M5 12h14M5 17h9' },
  history: { path: 'M4.5 12a7.5 7.5 0 1 0 2.2-5.3M4.5 4v4.2h4.2M12 8v4.5l3 2' },
  settings: { path: 'M10 3h4l.7 2.5 2 .9 2.3-.7 2 3.4-1.8 1.8v2.2l1.8 1.8-2 3.4-2.3-.7-2 .9L14 21h-4l-.7-2.5-2-.9-2.3.7-2-3.4 1.8-1.8v-2.2L3 9.1l2-3.4 2.3.7 2-.9zM12 8.5a3.5 3.5 0 1 0 0 7 3.5 3.5 0 0 0 0-7' },
  menu: { path: 'M4 6h16M4 12h16M4 18h16' },
  fit: { path: 'M4 9V4h5M15 4h5v5M20 15v5h-5M9 20H4v-5' },
  target: { path: 'M12 3v5M12 16v5M3 12h5M16 12h5M12 7a5 5 0 1 0 0 10 5 5 0 1 0 0-10' },
  edit: { path: 'M4.5 19.5l1-4.5L16 4.5l3.5 3.5L9 18.5zM14 6.5l3.5 3.5' },
  undo: { path: 'M9 6.5 4.5 11 9 15.5M4.5 11h10a4.5 4.5 0 0 1 0 9h-3' },
  redo: { path: 'M15 6.5l4.5 4.5-4.5 4.5M19.5 11h-10a4.5 4.5 0 0 0 0 9h3' },
  search: { path: 'M10.5 4a6.5 6.5 0 1 0 0 13 6.5 6.5 0 1 0 0-13M15.5 15.5 20 20' },
  backspace: { path: 'M9 5.5h11.5v13H9L3.5 12zM12 9.5l5 5M17 9.5l-5 5' },
} satisfies Record<string, Icon>

export type IconName = keyof typeof table

export const icons: Readonly<Record<IconName, Icon>> = table
