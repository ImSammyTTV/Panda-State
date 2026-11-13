#!/usr/bin/env python3
"""
A meter add-on for the Panda Status bar: show how much of something has been used.

It runs on any computer on the same network as the bar (Python 3, nothing to
install). Every so often it works out a number from 0 to 100 and posts it to
the bar, which draws it as a meter. Whether the bar actually shows it is the
owner's choice, on the bar's page under Add-ons.

Where the number comes from is up to you. Give it a command that prints a
number and this script does the rest:

    python3 meter.py --bar 192.168.1.50 --id disk --name "Disk used" \
        --colour "#3B82F6" --command "df --output=pcent / | tail -1"

Two meters side by side (for example a short-term and a weekly allowance):
repeat --command and --colour.

    python3 meter.py --bar 192.168.1.50 --id usage --name "Usage" \
        --command "cat /tmp/five-hour.txt" --colour "#D97757" \
        --command "cat /tmp/weekly.txt"    --colour "#3B82F6"

The bar forgets a post after its "ttl", so if this script stops, the meter
goes away by itself rather than showing a number that is no longer true.
"""
import argparse, json, re, subprocess, sys, time, urllib.error, urllib.request


def read_number(command):
    """Run the command and take the first number in what it prints. None if there is none: a missing reading is never shown as zero."""
    try:
        out = subprocess.run(command, shell=True, capture_output=True, text=True, timeout=30).stdout
    except (subprocess.SubprocessError, OSError):
        return None
    found = re.search(r'-?\d+(?:\.\d+)?', out)
    return max(0.0, min(100.0, float(found.group()))) if found else None


def post(bar, body):
    request = urllib.request.Request(f'http://{bar}/api/addon', data=json.dumps(body).encode(),
                                     headers={'content-type': 'application/json'})
    try:
        return json.load(urllib.request.urlopen(request, timeout=8))
    except urllib.error.HTTPError as error:
        return json.load(error)
    except (urllib.error.URLError, OSError) as error:
        return {'ok': False, 'message': str(error)}


def main():
    parser = argparse.ArgumentParser(description='Post one to four meters to a Panda Status bar.')
    parser.add_argument('--bar', required=True, help="the bar's address, e.g. 192.168.1.50")
    parser.add_argument('--id', required=True, help='short name for this add-on: a-z, 0-9, - and _, up to 15')
    parser.add_argument('--name', help="what the bar's page calls it")
    parser.add_argument('--command', action='append', required=True, help='a command that prints a number from 0 to 100; repeat for up to four meters')
    parser.add_argument('--colour', action='append', default=[], help='#RRGGBB for each meter, in the same order')
    parser.add_argument('--every', type=int, default=60, help='seconds between readings (default 60)')
    parser.add_argument('--once', action='store_true', help='post one reading and stop')
    args = parser.parse_args()
    if len(args.command) > 4:
        sys.exit('The bar draws at most four meters.')
    colours = args.colour + ['#FFFFFF'] * (len(args.command) - len(args.colour))

    while True:
        values = [read_number(command) for command in args.command]
        if any(value is None for value in values):
            # Say so here and post nothing; the bar drops the last reading when its ttl runs out.
            print('no number from:', [c for c, v in zip(args.command, values) if v is None], flush=True)
        else:
            answer = post(args.bar, {
                'id': args.id, 'name': args.name or args.id,
                # Long enough to ride out two missed readings, short enough that a dead script clears itself.
                'ttl': args.every * 3,
                'meters': [{'value': value, 'colour': colour} for value, colour in zip(values, colours)],
            })
            where = 'on the bar now' if answer.get('picked') else "posted (pick it on the bar's page under Add-ons to see it)"
            print(values, where if answer.get('ok') else 'refused: ' + str(answer.get('message')), flush=True)
        if args.once:
            return
        time.sleep(args.every)


if __name__ == '__main__':
    main()
