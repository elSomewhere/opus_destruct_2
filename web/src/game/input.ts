/**
 * Keyboard + mouse with pointer lock. Gameplay input is only read while the pointer is
 * locked, so typing in the settings panel never moves the player.
 * Key names are KeyboardEvent.code values (layout independent).
 */

export class Input {
  private readonly canvas: HTMLCanvasElement;
  private readonly down = new Set<string>();
  private readonly pressed = new Set<string>();
  private dx = 0;
  private dy = 0;
  private wheel = 0;
  private fireDown = false;
  private firePressed = false;
  locked = false;

  constructor(canvas: HTMLCanvasElement, onLockChange: (locked: boolean) => void) {
    this.canvas = canvas;
    document.addEventListener('pointerlockchange', () => {
      this.locked = document.pointerLockElement === canvas;
      if (!this.locked) {
        this.down.clear();
        this.fireDown = false;
      }
      onLockChange(this.locked);
    });
    document.addEventListener('pointerlockerror', () => onLockChange(false));
    canvas.addEventListener('mousedown', (e) => {
      if (!this.locked) {
        this.requestLock();
        return;
      }
      if (e.button === 0) {
        this.fireDown = true;
        this.firePressed = true;
      }
    });
    window.addEventListener('mouseup', (e) => {
      if (e.button === 0) this.fireDown = false;
    });
    document.addEventListener('mousemove', (e) => {
      if (!this.locked) return;
      this.dx += e.movementX;
      this.dy += e.movementY;
    });
    canvas.addEventListener(
      'wheel',
      (e) => {
        if (!this.locked) return;
        e.preventDefault();
        this.wheel += Math.sign(e.deltaY);
      },
      { passive: false },
    );
    window.addEventListener('keydown', (e) => {
      if (!this.locked) return;
      if (!e.repeat) this.pressed.add(e.code);
      this.down.add(e.code);
      if (e.code === 'Space' || e.code.startsWith('Arrow')) e.preventDefault();
    });
    window.addEventListener('keyup', (e) => this.down.delete(e.code));
    window.addEventListener('blur', () => {
      this.down.clear();
      this.fireDown = false;
    });
  }

  requestLock(): void {
    // unadjustedMovement (raw mouse) is not supported everywhere; fall back quietly.
    const c = this.canvas as HTMLCanvasElement & {
      requestPointerLock(options?: { unadjustedMovement?: boolean }): Promise<void> | void;
    };
    try {
      const r = c.requestPointerLock({ unadjustedMovement: true });
      if (r instanceof Promise) r.catch(() => void Promise.resolve(c.requestPointerLock()).catch(() => undefined));
    } catch {
      void Promise.resolve(c.requestPointerLock()).catch(() => undefined);
    }
  }

  isDown(code: string): boolean {
    return this.down.has(code);
  }

  /** True once per key press (edge). */
  wasPressed(code: string): boolean {
    return this.pressed.has(code);
  }

  get fireHeld(): boolean {
    return this.fireDown;
  }

  get fireClicked(): boolean {
    return this.firePressed;
  }

  consumeMouse(): [number, number] {
    const r: [number, number] = [this.dx, this.dy];
    this.dx = 0;
    this.dy = 0;
    return r;
  }

  consumeWheel(): number {
    const w = this.wheel;
    this.wheel = 0;
    return w;
  }

  /** Clears per-frame edges; call at the end of each frame. */
  endFrame(): void {
    this.pressed.clear();
    this.firePressed = false;
  }
}
