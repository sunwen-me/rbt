#!/usr/bin/env python3
"""Generate deterministic synthetic navigation maps and their scenario catalog.

These maps are software fixtures, not SLAM outputs or surveyed measurements.
Run from the repository root with:

    python3 navigation/scripts/generate_complex_maps.py
"""

from __future__ import annotations

import heapq
import json
import math
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAP_DIR = ROOT / "maps"
WIDTH = 120
HEIGHT = 80
RESOLUTION = 0.1
FREE = 254
OCCUPIED = 0
UNKNOWN = 205
PLANNING_RADIUS = 0.28


def blank_map() -> list[list[int]]:
    grid = [[FREE for _ in range(WIDTH)] for _ in range(HEIGHT)]
    rect(grid, 0, 0, WIDTH, 2, OCCUPIED)
    rect(grid, 0, HEIGHT - 2, WIDTH, HEIGHT, OCCUPIED)
    rect(grid, 0, 0, 2, HEIGHT, OCCUPIED)
    rect(grid, WIDTH - 2, 0, WIDTH, HEIGHT, OCCUPIED)
    return grid


def rect(
    grid: list[list[int]], x0: int, y0: int, x1: int, y1: int, value: int
) -> None:
    for y in range(max(0, y0), min(HEIGHT, y1)):
        for x in range(max(0, x0), min(WIDTH, x1)):
            grid[y][x] = value


def make_rooms() -> list[list[int]]:
    grid = blank_map()

    # Three staggered room partitions create door-to-door turns along the route.
    rect(grid, 36, 2, 38, 78, OCCUPIED)
    rect(grid, 36, 17, 38, 31, FREE)
    rect(grid, 36, 35, 118, 37, OCCUPIED)
    rect(grid, 56, 35, 70, 37, FREE)
    rect(grid, 84, 37, 86, 78, OCCUPIED)
    rect(grid, 84, 51, 86, 65, FREE)

    # A side-room partition enriches the layout without narrowing the route.
    rect(grid, 2, 58, 36, 60, OCCUPIED)
    rect(grid, 15, 58, 28, 60, FREE)

    # Unobserved fixture cells in the unused upper-left room.
    rect(grid, 4, 66, 14, 74, UNKNOWN)
    return grid


def make_warehouse() -> list[list[int]]:
    grid = blank_map()

    # Synthetic rack footprints: a low cross-row, a short central rack, and a
    # tall right rack. Their staggered ends produce a clear aisle with turns.
    rect(grid, 2, 20, 36, 26, OCCUPIED)
    rect(grid, 52, 26, 62, 45, OCCUPIED)
    rect(grid, 80, 2, 88, 58, OCCUPIED)

    # Free-standing pallet-stack/pillar footprints in side aisles.
    rect(grid, 20, 46, 24, 50, OCCUPIED)
    rect(grid, 94, 28, 98, 32, OCCUPIED)
    rect(grid, 68, 12, 72, 16, OCCUPIED)

    # An unobserved corner patch, away from the intended route.
    rect(grid, 4, 66, 14, 74, UNKNOWN)
    return grid


def write_map(stem: str, grid: list[list[int]]) -> None:
    pgm_path = MAP_DIR / f"{stem}.pgm"
    yaml_path = MAP_DIR / f"{stem}.yaml"

    # PGM rows run top-to-bottom; grid rows use map coordinates from the bottom.
    with pgm_path.open("w", encoding="ascii", newline="\n") as stream:
        stream.write("P2\n")
        stream.write("# Synthetic fixture map; not a surveyed or SLAM result.\n")
        stream.write(f"{WIDTH} {HEIGHT}\n255\n")
        for row in reversed(grid):
            stream.write(" ".join(str(value) for value in row) + "\n")

    yaml_path.write_text(
        "# Synthetic fixture map; not a surveyed or SLAM result.\n"
        f"image: {stem}.pgm\n"
        "mode: trinary\n"
        f"resolution: {RESOLUTION:.1f}\n"
        "origin: [0.0, 0.0, 0.0]\n"
        "negate: 0\n"
        "occupied_thresh: 0.65\n"
        "free_thresh: 0.196\n",
        encoding="ascii",
    )


def inflated_free(grid: list[list[int]]) -> list[list[bool]]:
    # Account for the obstacle cell's half diagonal when conservatively
    # approximating the unchanged 0.28 m robot-plus-clearance radius.
    threshold = PLANNING_RADIUS + math.sqrt(2) * RESOLUTION / 2
    reach = math.ceil(threshold / RESOLUTION)
    free = [[value == FREE for value in row] for row in grid]
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if not free[y][x]:
                continue
            for oy in range(max(0, y - reach), min(HEIGHT, y + reach + 1)):
                for ox in range(max(0, x - reach), min(WIDTH, x + reach + 1)):
                    if grid[oy][ox] != FREE:
                        distance = math.hypot(x - ox, y - oy) * RESOLUTION
                        if distance <= threshold:
                            free[y][x] = False
                            break
                if not free[y][x]:
                    break
    return free


def shortest_path(
    free: list[list[bool]], start: tuple[int, int], goal: tuple[int, int]
) -> tuple[list[tuple[int, int]], float]:
    sx, sy = start
    gx, gy = goal
    if not free[sy][sx] or not free[gy][gx]:
        raise ValueError(f"start or goal is blocked: {start}, {goal}")

    distances = {start: 0.0}
    previous: dict[tuple[int, int], tuple[int, int]] = {}
    queue = [(0.0, start)]
    steps = [
        (-1, 0, 1.0), (1, 0, 1.0), (0, -1, 1.0), (0, 1, 1.0),
        (-1, -1, math.sqrt(2)), (-1, 1, math.sqrt(2)),
        (1, -1, math.sqrt(2)), (1, 1, math.sqrt(2)),
    ]

    while queue:
        distance, point = heapq.heappop(queue)
        if distance != distances[point]:
            continue
        if point == goal:
            break
        x, y = point
        for dx, dy, cost in steps:
            nx, ny = x + dx, y + dy
            if not (0 <= nx < WIDTH and 0 <= ny < HEIGHT and free[ny][nx]):
                continue
            if dx and dy and (not free[y][nx] or not free[ny][x]):
                continue
            neighbor = (nx, ny)
            candidate = distance + cost * RESOLUTION
            if candidate < distances.get(neighbor, math.inf):
                distances[neighbor] = candidate
                previous[neighbor] = point
                heapq.heappush(queue, (candidate, neighbor))

    if goal not in distances:
        raise ValueError(f"no inflated-grid path from {start} to {goal}")
    path = [goal]
    while path[-1] != start:
        path.append(previous[path[-1]])
    path.reverse()
    return path, distances[goal]


def direct_line_clear(
    free: list[list[bool]], start: tuple[int, int], goal: tuple[int, int]
) -> bool:
    sx, sy = start
    gx, gy = goal
    x0, y0 = (sx + 0.5) * RESOLUTION, (sy + 0.5) * RESOLUTION
    x1, y1 = (gx + 0.5) * RESOLUTION, (gy + 0.5) * RESOLUTION
    length = math.hypot(x1 - x0, y1 - y0)
    samples = math.ceil(length / (RESOLUTION / 4))
    for index in range(samples + 1):
        ratio = index / samples
        x = min(WIDTH - 1, int((x0 + ratio * (x1 - x0)) / RESOLUTION))
        y = min(HEIGHT - 1, int((y0 + ratio * (y1 - y0)) / RESOLUTION))
        if not free[y][x]:
            return False
    return True


def nearest_obstacle_clearance(
    grid: list[list[int]], point: tuple[int, int]
) -> float:
    x, y = point
    obstacle_cells = [
        (ox, oy)
        for oy, row in enumerate(grid)
        for ox, value in enumerate(row)
        if value != FREE
    ]
    return min(math.hypot(x - ox, y - oy) * RESOLUTION for ox, oy in obstacle_cells) - RESOLUTION / 2


def cell_center(index: int) -> float:
    return round((index + 0.5) * RESOLUTION, 2)


def verify_scene(
    name: str,
    grid: list[list[int]],
    start: tuple[int, int],
    goal: tuple[int, int],
    min_passage: float,
) -> float:
    free = inflated_free(grid)
    path, distance = shortest_path(free, start, goal)
    if direct_line_clear(free, start, goal):
        raise ValueError(f"{name}: direct start-to-goal line is unexpectedly clear")
    for point, label in ((start, "start"), (goal, "goal")):
        clearance = nearest_obstacle_clearance(grid, point)
        if clearance < 0.5:
            raise ValueError(f"{name}: {label} clearance is only {clearance:.2f} m")
    if not 8.0 <= distance <= 12.0:
        raise ValueError(f"{name}: inflated route is {distance:.2f} m, outside 8-12 m")
    if min_passage < 1.1:
        raise ValueError(f"{name}: intended passage is too narrow ({min_passage:.2f} m)")
    print(
        f"{name}: {WIDTH}x{HEIGHT} cells ({WIDTH * RESOLUTION:.1f}x"
        f"{HEIGHT * RESOLUTION:.1f} m), path={distance:.2f} m, "
        f"direct_line=blocked, min_intended_passage={min_passage:.1f} m, "
        f"start_clearance={nearest_obstacle_clearance(grid, start):.2f} m, "
        f"goal_clearance={nearest_obstacle_clearance(grid, goal):.2f} m, "
        f"waypoints={len(path)} cells"
    )
    return distance


def main() -> None:
    MAP_DIR.mkdir(parents=True, exist_ok=True)
    rooms = make_rooms()
    warehouse = make_warehouse()

    room_start = (12, 12)
    room_goal = (104, 68)
    warehouse_start = (12, 10)
    warehouse_goal = (94, 68)

    verify_scene("rooms", rooms, room_start, room_goal, min_passage=1.4)
    verify_scene("warehouse", warehouse, warehouse_start, warehouse_goal, min_passage=1.6)
    write_map("complex_rooms", rooms)
    write_map("complex_warehouse", warehouse)

    scenarios = {
        "source": "Synthetic fixture maps; neither scene is a SLAM result.",
        "scenarios": [
            {
                "id": "rooms",
                "name": "多房间门洞转弯",
                "map": "complex_rooms.yaml",
                "start": [cell_center(room_start[0]), cell_center(room_start[1]), 0.0],
                "goal": [cell_center(room_goal[0]), cell_center(room_goal[1]), 0.0],
                "goal_timeout_seconds": 90,
            },
            {
                "id": "warehouse",
                "name": "仓库货架与立柱绕行",
                "map": "complex_warehouse.yaml",
                "start": [
                    cell_center(warehouse_start[0]),
                    cell_center(warehouse_start[1]),
                    0.0,
                ],
                "goal": [
                    cell_center(warehouse_goal[0]),
                    cell_center(warehouse_goal[1]),
                    0.0,
                ],
                "goal_timeout_seconds": 90,
            },
        ],
    }
    (MAP_DIR / "complex_scenarios.json").write_text(
        json.dumps(scenarios, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
