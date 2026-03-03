"""
bot_base.py — Abstract base class for bots.

Provides all connection, authentication, and protocol-dispatch logic.
Subclass :class:`BotBase` and implement :meth:`BotBase.make_move` to build
your own bot.

Architecture
------------
  * Main thread        — connects, authenticates, then just blocks on join()
  * Reader thread      — reads lines from the socket and dispatches;
                         never calls make_move() directly
  * Writer thread      — drains the outbound queue and sends lines
  * Per-game thread    — one daemon thread per active game; blocks on a
                         per-game queue and calls make_move() in isolation

Parallelism: each game's move computation runs in its own thread, so a slow
search in game A never delays YOUR_TURN processing or HEARTBEAT_ACK for game B.
All threads share a single TCP connection via the thread-safe _send_queue.
"""

from __future__ import annotations

import queue
import socket
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Optional


# ─────────────────────────────────────────────────────────────────────────────
# Minimal board state (for move generation)
# ─────────────────────────────────────────────────────────────────────────────

BOARD_SIZE    = 9
INITIAL_WALLS = 10
WALL_H, WALL_V = "H", "V"
P1, P2 = "P1", "P2"
GOAL_ROW = {P1: 0, P2: 8}


@dataclass
class LocalBoard:
    """Lightweight board state parsed from a YOUR_TURN message."""
    my_side:   str = P1
    p1_pos:    tuple[int, int] = (8, 4)
    p2_pos:    tuple[int, int] = (0, 4)
    walls_left: dict = field(default_factory=lambda: {P1: 10, P2: 10})
    h_walls:   set   = field(default_factory=set)
    v_walls:   set   = field(default_factory=set)

    # ── Helpers ───────────────────────────────────────────────────────────────

    @property
    def my_pos(self) -> tuple[int, int]:
        return self.p1_pos if self.my_side == P1 else self.p2_pos

    @property
    def opp_pos(self) -> tuple[int, int]:
        return self.p2_pos if self.my_side == P1 else self.p1_pos

    def _blocks_south(self, r: int, c: int) -> bool:
        return (r, c) in self.h_walls or (r, c - 1) in self.h_walls

    def _blocks_east(self, r: int, c: int) -> bool:
        return (r, c) in self.v_walls or (r - 1, c) in self.v_walls

    def _can_step(self, fr: int, fc: int, tr: int, tc: int) -> bool:
        if not (0 <= tr < BOARD_SIZE and 0 <= tc < BOARD_SIZE):
            return False
        dr, dc = tr - fr, tc - fc
        if abs(dr) + abs(dc) != 1:
            return False
        if dr == 1:  return not self._blocks_south(fr, fc)
        if dr == -1: return not self._blocks_south(tr, tc)
        if dc == 1:  return not self._blocks_east(fr, fc)
        return not self._blocks_east(tr, tc)

    def _has_path(self, start: tuple[int, int], goal_row: int, opp: tuple[int, int]) -> bool:
        from collections import deque
        visited = {start}
        q: deque = deque([start])
        while q:
            r, c = q.popleft()
            if r == goal_row:
                return True
            for dr, dc in [(-1,0),(1,0),(0,-1),(0,1)]:
                nr, nc = r+dr, c+dc
                if (nr,nc) in visited: continue
                if not self._can_step(r,c,nr,nc): continue
                visited.add((nr,nc))
                q.append((nr,nc))
        return False

    def legal_moves(self) -> list[tuple[int, int]]:
        pr, pc  = self.my_pos
        or_, oc = self.opp_pos
        result  = []
        for dr, dc in [(-1,0),(1,0),(0,-1),(0,1)]:
            nr, nc = pr+dr, pc+dc
            if not self._can_step(pr, pc, nr, nc):
                continue
            if (nr, nc) == (or_, oc):
                jr, jc = nr+dr, nc+dc
                if self._can_step(nr,nc,jr,jc):
                    result.append((jr,jc))
                else:
                    for ddr,ddc in [(-dc,dr),(dc,-dr)]:
                        sr,sc = nr+ddr, nc+ddc
                        if self._can_step(nr,nc,sr,sc):
                            result.append((sr,sc))
            else:
                result.append((nr,nc))
        return result

    def legal_walls(self) -> list[tuple[int, int, str]]:
        """Return a sampled subset of legal wall placements (not exhaustive, to save time)."""
        my_walls = self.walls_left.get(self.my_side, 0)
        if my_walls <= 0:
            return []
        candidates = []
        for r in range(8):
            for c in range(8):
                for ori in (WALL_H, WALL_V):
                    if self._wall_conflicts(ori, r, c):
                        continue
                    # Path check
                    test = LocalBoard(
                        my_side    = self.my_side,
                        p1_pos     = self.p1_pos,
                        p2_pos     = self.p2_pos,
                        walls_left = dict(self.walls_left),
                        h_walls    = set(self.h_walls),
                        v_walls    = set(self.v_walls),
                    )
                    if ori == WALL_H:
                        test.h_walls.add((r,c))
                    else:
                        test.v_walls.add((r,c))
                    if (test._has_path(test.p1_pos, GOAL_ROW[P1], test.p2_pos) and
                            test._has_path(test.p2_pos, GOAL_ROW[P2], test.p1_pos)):
                        candidates.append((r, c, ori))
        return candidates

    def _wall_conflicts(self, orientation: str, r: int, c: int) -> bool:
        if orientation == WALL_H:
            if (r,c) in self.h_walls: return True
            if (r,c-1) in self.h_walls or (r,c+1) in self.h_walls: return True
            if (r,c) in self.v_walls: return True
        else:
            if (r,c) in self.v_walls: return True
            if (r-1,c) in self.v_walls or (r+1,c) in self.v_walls: return True
            if (r,c) in self.h_walls: return True
        return False

    def visualize(self, print_to_console: bool = False) -> str:
        """
        Render the board as an ASCII string.

        Cell contents:
            1  — P1 pawn
            2  — P2 pawn
            .  — empty cell

        Wall markers (between cells):
            |  — vertical wall blocking east movement
            =  — horizontal wall blocking south movement
            +  — junction where a wall starts or ends

        The column and row indices are printed as headers.

        Example (starting position, no walls):
            0 1 2 3 4 5 6 7 8
          0 . . . . 2 . . . .
            . . . . . . . . .
          1 . . . . . . . . .
            ...
          8 . . . . 1 . . . .

        With a horizontal wall at anchor (3,4) shown as '═' beneath row 3:
            col 4 and 5 have '=' under row 3, with '+' at the joints.
        """
        lines: list[str] = []

        # Column header
        lines.append("   " + " ".join(str(c) for c in range(BOARD_SIZE)))

        for r in range(BOARD_SIZE):
            # ── Cell row ──────────────────────────────────────────────────────
            cell_row: list[str] = []
            for c in range(BOARD_SIZE):
                if (r, c) == self.p1_pos:
                    cell_row.append("1")
                elif (r, c) == self.p2_pos:
                    cell_row.append("2")
                else:
                    cell_row.append(".")
                # Vertical wall separator between column c and c+1
                if c < BOARD_SIZE - 1:
                    cell_row.append("|" if self._blocks_east(r, c) else " ")
            lines.append(f"{r:2} " + "".join(cell_row))

            # ── Wall row between row r and row r+1 ───────────────────────────
            if r < BOARD_SIZE - 1:
                wall_row: list[str] = []
                for c in range(BOARD_SIZE):
                    h_blocked = self._blocks_south(r, c)
                    wall_row.append("=" if h_blocked else " ")
                    if c < BOARD_SIZE - 1:
                        # Show '+' at a junction if any adjacent wall segment exists
                        any_wall = (
                            h_blocked
                            or self._blocks_south(r, c + 1)
                            or self._blocks_east(r, c)
                            or self._blocks_east(r + 1, c)
                        )
                        wall_row.append("+" if any_wall else " ")
                lines.append("   " + "".join(wall_row))

        result = "\n".join(lines)
        if print_to_console:
            print(result)
        return result


def parse_board(board_str: str, my_side: str) -> LocalBoard:
    """Parse a board-state string from a YOUR_TURN message."""
    parts = {}
    for token in board_str.split():
        k, _, v = token.partition(":")
        parts[k] = v

    def pp(s):
        r, c = s.split(",")
        return int(r), int(c)

    def pw(s):
        if not s:
            return set()
        return {pp(w) for w in s.split(";")}

    wl = parts["wl"].split(",")
    return LocalBoard(
        my_side    = my_side,
        p1_pos     = pp(parts["p1"]),
        p2_pos     = pp(parts["p2"]),
        walls_left = {P1: int(wl[0]), P2: int(wl[1])},
        h_walls    = pw(parts["hw"]),
        v_walls    = pw(parts["vw"]),
    )


# ─────────────────────────────────────────────────────────────────────────────
# Bot base class
# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class GameState:
    game_id:   str
    my_side:   str
    opponent:  str
    move_queue: queue.Queue = field(default_factory=queue.Queue)
    worker:    Optional[threading.Thread] = field(default=None)


class BotBase:
    """Abstract base class for bots.

    Handles all connection, authentication, and protocol logic.
    Subclasses must implement :meth:`make_move`.
    """

    def __init__(self, host: str, port: int, token: str) -> None:
        self._host   = host
        self._port   = port
        self._token  = token
        self._sock: Optional[socket.socket] = None
        self._send_queue: queue.Queue = queue.Queue()
        self._games: dict[str, GameState] = {}
        self._name  = ""
        self._version = 0
        self._stop  = threading.Event()

    # ── Connection ────────────────────────────────────────────────────────────

    def connect(self) -> None:
        self._sock = socket.create_connection((self._host, self._port))
        self._sock.settimeout(None)  # blocking mode
        print(f"[bot] connected to {self._host}:{self._port}")

    def run(self) -> None:
        self.connect()
        assert self._sock is not None
        sock = self._sock
        writer_thread = threading.Thread(target=self._writer, args=(sock,), daemon=True)
        writer_thread.start()
        self._reader(sock)   # blocks until connection closes

    # ── Writer thread ─────────────────────────────────────────────────────────

    def _writer(self, sock: socket.socket) -> None:
        """Drain the outbound queue and send each line to the server."""
        f = sock.makefile("wb")
        while not self._stop.is_set():
            try:
                line = self._send_queue.get(timeout=0.5)
                f.write((line + "\n").encode())
                f.flush()
            except queue.Empty:
                continue
            except (BrokenPipeError, OSError):
                self._stop.set()
                break

    def _send(self, line: str) -> None:
        self._send_queue.put(line)

    # ── Reader thread (main) ──────────────────────────────────────────────────

    def _reader(self, sock: socket.socket) -> None:
        f = sock.makefile("r", encoding="utf-8", errors="replace")
        for raw in f:
            line = raw.strip()
            if not line:
                continue
            parts = line.split()
            cmd   = parts[0].upper()
            args  = parts[1:]
            self._dispatch(cmd, args)
        self._stop.set()
        print("[bot] connection closed by server")

    def _dispatch(self, cmd: str, args: list[str]) -> None:
        print(f"[bot] << {cmd} {' '.join(args)}")

        if cmd == "HELLO":
            self._send(f"AUTH {self._token}")

        elif cmd == "AUTH_OK":
            self._name    = args[0] if args else "?"
            self._version = int(args[1]) if len(args) > 1 else 0
            print(f"[bot] authenticated as {self._name} v{self._version}")

        elif cmd == "AUTH_FAIL":
            reason = args[0] if args else "?"
            print(f"[bot] auth failed: {reason}", file=sys.stderr)
            self._stop.set()

        elif cmd == "HEARTBEAT":
            self._send("HEARTBEAT_ACK")

        elif cmd == "GAME_START":
            # GAME_START <game_id> <opponent> <your_side> <first_mover> <walls>
            if len(args) < 5:
                return
            game_id, opponent, my_side = args[0], args[1], args[2]
            state = GameState(game_id, my_side, opponent)
            state.worker = threading.Thread(
                target=self._game_worker,
                args=(state,),
                daemon=True,
                name=f"game-{game_id[:8]}",
            )
            self._games[game_id] = state
            state.worker.start()
            print(f"[bot] game started: {game_id}  ({my_side} vs {opponent})")

        elif cmd == "YOUR_TURN":
            # YOUR_TURN <game_id> <board_state...>
            if len(args) < 2:
                return
            game_id = args[0]
            board_str = " ".join(args[1:])
            game = self._games.get(game_id)
            if game is None:
                return
            game.move_queue.put(board_str)  # hand off; reader returns immediately

        elif cmd == "MOVE_OK":
            pass  # acknowledged

        elif cmd == "GAME_END":
            # GAME_END <game_id> <result> <reason>
            game_id = args[0] if args else "?"
            result  = args[1] if len(args) > 1 else "?"
            reason  = args[2] if len(args) > 2 else "?"
            print(f"[bot] game {game_id} ended — {result} ({reason})")
            game = self._games.pop(game_id, None)
            if game is not None:
                game.move_queue.put(None)          # sentinel — stop worker
                if game.worker is not None:
                    game.worker.join(timeout=2.0)  # reap thread; avoid leaks

        elif cmd == "ERROR":
            print(f"[bot] server error: {' '.join(args)}", file=sys.stderr)

    # ── Per-game worker ────────────────────────────────────────────────────────

    def _game_worker(self, state: GameState) -> None:
        """Dedicated thread for one game — serialises make_move() calls."""
        while True:
            board_str = state.move_queue.get()
            if board_str is None:  # sentinel from GAME_END
                break
            self.make_move(state.game_id, board_str, state.my_side)

    # ── Move generation ───────────────────────────────────────────────────────

    def make_move(self, game_id: str, board_str: str, my_side: str) -> None:
        """Decide and send a move for the given game.

        Called whenever the server sends YOUR_TURN for *game_id*.
        Use ``self._send(line)`` to transmit a ``MOVE`` or ``WALL`` command.

        Subclasses **must** override this method.
        """
        raise NotImplementedError("Subclasses must implement make_move")
 
