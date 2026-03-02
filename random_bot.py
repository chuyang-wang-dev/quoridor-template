"""
random_bot.py — Random-move bot (reference client).

This file is intentionally written using only the Python standard library
(socket, threading, queue, time, random) to serve as a guide for exercise
participants who can implement their player in any language.

Usage
-----
    python random_bot.py --token <TOKEN> [--host 127.0.0.1] [--port 5555]

    BOT_TOKEN environment variable is also accepted.
"""

from __future__ import annotations

import argparse
import os
import random
import sys

from bot_base import BotBase, parse_board


# ─────────────────────────────────────────────────────────────────────────────
# Random-move bot
# ─────────────────────────────────────────────────────────────────────────────

class RandomBot(BotBase):
    """Concrete bot that picks a random legal pawn move or wall placement."""

    def make_move(self, game_id: str, board_str: str, my_side: str) -> None:
        """Pick a random legal action and send it."""
        try:
            board = parse_board(board_str, my_side)
        except Exception as exc:
            print(f"[bot] board parse error: {exc}", file=sys.stderr)
            return

        moves = board.legal_moves()

        # With some probability, try to place a wall instead
        walls = []
        if board.walls_left.get(my_side, 0) > 0 and random.random() < 0.25:
            walls = board.legal_walls()

        if walls and random.random() < 0.5:
            r, c, ori = random.choice(walls)
            self._send(f"WALL {game_id} {r} {c} {ori}")
            print(f"[bot] >> WALL {game_id} {r} {c} {ori}")
        elif moves:
            r, c = random.choice(moves)
            self._send(f"MOVE {game_id} {r} {c}")
            print(f"[bot] >> MOVE {game_id} {r} {c}")
        else:
            # No legal moves (shouldn't happen in a valid game)
            print(f"[bot] WARNING: no legal moves for game {game_id}")


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="Random-move bot (reference client)")
    parser.add_argument("--host",  default="127.0.0.1", help="Server host (default: 127.0.0.1)")
    parser.add_argument("--port",  type=int, default=5555, help="Server port (default: 5555)")
    parser.add_argument("--token", default=os.getenv("BOT_TOKEN", ""), help="Auth token")
    args = parser.parse_args()

    if not args.token:
        print("Error: no token provided. Use --token or set BOT_TOKEN env var.", file=sys.stderr)
        sys.exit(1)

    bot = RandomBot(args.host, args.port, args.token)
    try:
        bot.run()
    except KeyboardInterrupt:
        print("[bot] interrupted")


if __name__ == "__main__":
    main()
