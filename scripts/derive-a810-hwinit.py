#!/usr/bin/env python3
"""Derive the Adreno 810 (gen8) register map and init tables from pinned mainline sources.

Offsets and bitfields come from the pinned register XMLs (a6xx.xml, a6xx_gmu.xml, adreno_common.xml) with the variant
the kernel's own header generator gives each C name: REG_A8XX_X is the definition whose variants start at A8XX,
REG_A6XX_X the one with no variants or starting at A6XX, and every definition used must also cover A8XX. The register
set is every REG_* name the gen8 paths of a8xx_gpu.c touch (hw_init, CP protect, UBWC, non-context config, hwcg,
IRQ, idle, flush, GBIF halt, aperture, timestamp), plus the ring registers this driver writes. Tables are the A810
entry of a6xx_catalog.c (a810_nonctxt_regs, a810_protect, a810_pwrup_reglist, a810_ifpc_reglist,
a810_dyn_pwrup_reglist, a840_gbif) and the constants of a8xx_gpu.c (interrupt and APRIV masks, CP protect control,
bicubic weights).

--fetch makes the sparse checkout in .cache/linux-a8xx; every run checks the pinned sha256 of each input first.
Writes NativeAdrenoA810HwInit.h; --check compares instead."""
import ast, hashlib, json, re, subprocess, sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PIN = json.loads((ROOT / 'scripts/a810-inputs.json').read_text())['linux']
SRC = ROOT / '.cache/linux-a8xx'
MSM = SRC / 'drivers/gpu/drm/msm'
OUT = ROOT / 'NativeAdrenoA810HwInit.h'
NS = '{http://nouveau.freedesktop.org/}'
ORDER = ['A2XX', 'A3XX', 'A4XX', 'A5XX', 'A6XX', 'A7XX', 'A8XX']
TARGET = 'A8XX'
GPU_XML = ['a6xx.xml', 'adreno_common.xml']
GMU_XML = ['a6xx_gmu.xml']
# a8xx_gpu.c functions whose register uses this driver needs.
FUNCTIONS = ['a8xx_aperture_slice_set', 'a8xx_set_hwcg', 'a8xx_set_cp_protect', 'a8xx_set_ubwc_config',
             'a8xx_nonctxt_config', 'a8xx_patch_pwrup_reglist', 'a8xx_cp_init', 'hw_init', '_a8xx_check_idle',
             'a8xx_idle', 'a8xx_flush', 'a8xx_cp_hw_err_irq', 'a8xx_irq', 'a8xx_bus_clear_pending_transactions',
             'a8xx_gmu_get_timestamp', 'a8xx_fault_handler']
MASKS = ['A8XX_INT_MASK', 'A8XX_APRIV_MASK', 'A8XX_BR_APRIV_MASK', 'A8XX_CP_GLOBAL_INT_MASK',
         'A8XX_CP_INTERRUPT_STATUS_MASK_PIPE', 'A8XX_CP_HW_FAULT_STATUS_MASK_PIPE']
# Registers the ring and timer modules use that the functions above do not name.
EXTRA = ['REG_A6XX_CP_RB_CNTL', 'REG_A6XX_CP_RB_RPTR', 'REG_A6XX_CP_RB_WPTR', 'REG_A8XX_CP_SCRATCH_GLOBAL', 'REG_A8XX_CP_SCRATCH_PIPE',
         'REG_A8XX_RBBM_PERFCTR_SRAM_INIT_CMD', 'REG_A8XX_RBBM_PERFCTR_SRAM_INIT_STATUS',
         'REG_A8XX_CP_ALWAYS_ON_COUNTER', 'REG_A8XX_CP_INTERRUPT_STATUS_PIPE', 'REG_A8XX_CP_HW_FAULT_STATUS_PIPE',
         'REG_A8XX_RBBM_GBIF_HALT_ACK', 'REG_A6XX_GBIF_HALT_ACK',
         'REG_A8XX_RBBM_SLICE_PERFCTR_SRAM_INIT_CMD', 'REG_A8XX_CP_ALWAYS_ON_CONTEXT', 'REG_A8XX_RBBM_PERFCTR_CP']
# Bitsets dumped whole, for the interrupt decoder.
DUMPS = [('A6XX_RBBM_INT_0_MASK', 'RBBM_INT_0'), ('A8XX_CP_GLOBAL_INT_MASK', 'CP_GLOBAL_INT')]


def fail(message):
    raise SystemExit('derive-a810-hwinit: ' + message)


def fetch():
    if not (SRC / '.git').exists():
        SRC.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', 'init', '-q', str(SRC)], check=True)
        subprocess.run(['git', '-C', str(SRC), 'remote', 'add', 'origin', PIN['repository']], check=True)
    subprocess.run(['git', '-C', str(SRC), 'sparse-checkout', 'set', '--no-cone',
                    '/drivers/gpu/drm/msm/adreno/', '/drivers/gpu/drm/msm/registers/adreno/'], check=True)
    subprocess.run(['git', '-C', str(SRC), 'fetch', '-q', '--depth', '1', '--filter=blob:none', 'origin',
                    PIN['commit']], check=True)
    subprocess.run(['git', '-C', str(SRC), 'checkout', '-q', 'FETCH_HEAD'], check=True)


def verify():
    for name, digest in PIN['files'].items():
        path = SRC / name
        if not path.exists():
            fail('missing %s; run with --fetch' % name)
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            fail('%s does not match its pin' % name)


def covers(variants):
    if variants is None:
        return True
    for part in variants.split(','):
        part = part.strip()
        if '-' in part:
            left, right = part.split('-', 1)
            low = ORDER.index(left) if left else 0
            high = ORDER.index(right) if right else len(ORDER) - 1
            if low <= ORDER.index(TARGET) <= high:
                return True
        elif part == TARGET:
            return True
    return False


def first_variant(variants, domain):
    if variants is None:
        return domain
    return re.split(r'[-,]', variants.strip())[0] or domain


class Database:
    def __init__(self, files):
        self.regs = {}     # name -> [(prefix, variants, offset, kind, stride, element)]
        self.bitsets = {}  # name -> element
        self.enums = {}    # name -> {value name: value}
        for name in files:
            tree = ET.parse(MSM / 'registers/adreno' / name).getroot()
            for element in tree.iter():
                tag = element.tag.replace(NS, '')
                if tag == 'bitset':
                    self.bitsets.setdefault(element.get('name'), element)
                elif tag == 'enum':
                    values = self.enums.setdefault(element.get('name'), {})
                    for value in element.findall(NS + 'value'):
                        values[value.get('name')] = int(value.get('value'), 0)
            for domain in tree.findall(NS + 'domain'):
                for element in domain:
                    tag = element.tag.replace(NS, '')
                    if tag not in ('reg32', 'reg64', 'array'):
                        continue
                    offset = int(element.get('offset'), 0)
                    stride = int(element.get('stride', '0'), 0)
                    if tag == 'array':
                        child = element.find(NS + 'reg32')
                        if child is None:
                            child = element.find(NS + 'reg64')
                        offset += int(child.get('offset'), 0) if child is not None else 0
                    variants = element.get('variants')
                    self.regs.setdefault(element.get('name'), []).append(
                        (first_variant(variants, domain.get('name')), variants, offset, tag, stride, element))

    def reg(self, prefix, name, strict=True):
        hits = [r for r in self.regs.get(name, []) if r[0] == prefix]
        if len({(r[2], r[3], r[4]) for r in hits}) != 1:
            return None
        if not covers(hits[0][1]):
            if strict:
                fail('REG_%s_%s does not cover %s' % (prefix, name, TARGET))
            return None
        return hits[0]

    def fields(self, element):
        """Bitfields valid for the target, from the register's inline fields or its type's bitset."""
        found = {}
        sources = [element]
        if element.get('type') in self.bitsets:
            sources.append(self.bitsets[element.get('type')])
        for source in sources:
            for field in source.findall(NS + 'bitfield'):
                if covers(field.get('variants')):
                    if field.get('pos') is not None:
                        low = high = int(field.get('pos'))
                    else:
                        low, high = int(field.get('low')), int(field.get('high'))
                    found.setdefault(field.get('name'), (low, high))
        return found


def function_body(text, name):
    m = re.search(r'^[a-z][^\n;]*\b' + re.escape(name) + r'\([^;{]*\)\s*\{', text, re.M)
    if not m:
        fail('a8xx_gpu.c has no function ' + name)
    depth, i = 1, m.end()
    while depth:
        depth += {'{': 1, '}': -1}.get(text[i], 0)
        i += 1
    return text[m.start():i]


def strip_comments(text):
    return re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)


def evaluate(expression, names):
    """Integer constant expression of the catalog: numbers, BIT(), SZ_*, named constants, | + * << ( )."""
    expression = re.sub(r'\bBIT\(', '(1<<', expression)
    expression = re.sub(r'\bGENMASK\((\d+),\s*(\d+)\)', lambda m: str(((1 << (int(m.group(1)) - int(m.group(2)) + 1))
                                                                      - 1) << int(m.group(2))), expression)
    expression = expression.replace('SZ_1K', str(1 << 10)).replace('SZ_64K', str(1 << 16)).replace(
        'SZ_512K', str(1 << 19)).replace('SZ_64M', str(1 << 26))
    tree = ast.parse('(' + expression.strip() + ')', mode='eval')

    def walk(node):
        if isinstance(node, ast.Expression):
            return walk(node.body)
        if isinstance(node, ast.Constant) and isinstance(node.value, int):
            return node.value
        if isinstance(node, ast.Name):
            if node.id not in names:
                raise KeyError(node.id)
            return names[node.id]
        if isinstance(node, ast.BinOp) and type(node.op) in (ast.BitOr, ast.Add, ast.Mult, ast.LShift):
            left, right = walk(node.left), walk(node.right)
            return {ast.BitOr: left | right, ast.Add: left + right, ast.Mult: left * right,
                    ast.LShift: left << right}[type(node.op)]
        raise KeyError(ast.dump(node))
    return walk(tree)


class Resolver:
    def __init__(self):
        self.gpu = Database(GPU_XML)
        self.gmu = Database(GMU_XML)
        self.pipes = self.gpu.enums['adreno_pipe']
        self.used = {}  # C name -> (out name, offset, kind, stride, indexed, gmu)

    def register(self, prefix, name, gmu, indexed):
        database = self.gmu if gmu else self.gpu
        hit = database.reg(prefix, name)
        if hit is None and not gmu:
            hit, gmu = self.gmu.reg(prefix, name), True
        if hit is None:
            fail('REG_%s_%s is not one definition in the pinned XMLs' % (prefix, name))
        key = 'REG_%s_%s' % (prefix, name)
        entry = (name, hit[2], hit[3], hit[4], indexed or hit[3] == 'array', gmu)
        old = self.used.get(key)
        if old and old[1:4] != entry[1:4]:
            fail(key + ' resolved twice')
        if old:
            entry = entry[:4] + (old[4] or entry[4], old[5] or entry[5])
        self.used[key] = entry
        return hit

    def offset(self, token):
        m = re.fullmatch(r'REG_(A[678]XX)_([A-Z0-9_]+?)(?:\((\d+)\))?', token)
        if not m:
            fail('not a register: ' + token)
        name = m.group(2)
        gmu = name.startswith(('GMU_', 'GPU_GMU_'))
        hit = self.register(m.group(1), name, gmu, m.group(3) is not None)
        index = int(m.group(3)) if m.group(3) else 0
        if hit[3] == 'array':
            length = int(hit[5].get('length'), 0)
            if index >= length:
                fail('%s index %d is past length %d' % (token, index, length))
        return hit[2] + hit[4] * index

    def field(self, token):
        """A[678]XX_<bitset or register>_<FIELD>[__MASK|__SHIFT] -> (out name, low, high)."""
        suffix = ''
        for tail in ('__MASK', '__SHIFT'):
            if token.endswith(tail):
                token, suffix = token[:-len(tail)], tail
        best = None
        for name in self.gpu.bitsets:
            if token.startswith(name + '_'):
                fields = self.gpu.fields(self.gpu.bitsets[name])
                rest = token[len(name) + 1:]
                if rest in fields and (best is None or len(name) > best[0]):
                    best = (len(name), name[5:], rest, fields[rest])
        m = re.match(r'(A[678]XX)_(.+)$', token)
        if m:
            parts = m.group(2).split('_')
            for cut in range(len(parts) - 1, 0, -1):
                name, rest = '_'.join(parts[:cut]), '_'.join(parts[cut:])
                for database in (self.gpu, self.gmu):
                    hit = database.reg(m.group(1), name, strict=False)
                    if hit is None:
                        continue
                    fields = database.fields(hit[5])
                    if rest in fields and (best is None or len(m.group(1)) + 1 + len(name) > best[0]):
                        best = (len(m.group(1)) + 1 + len(name), name, rest, fields[rest])
        if best is None:
            fail('no field for ' + token)
        return best[1] + '_' + best[2], best[3], suffix

    def constant(self, expression, defines):
        names = dict(self.pipes)

        def substitute(m):
            token = m.group(0)
            if token.startswith('REG_'):
                return str(self.offset(token))
            if token in defines:
                return '(%d)' % defines[token]
            name, (low, high), suffix = self.field(token)
            if suffix == '__SHIFT':
                return str(low)
            return str(((1 << (high - low + 1)) - 1) << low)
        expression = re.sub(r'\bREG_A[678]XX_[A-Z0-9_]+(?:\(\d+\))?|\bA[678]XX_[A-Z0-9_]+', substitute, expression)
        return evaluate(expression, names)


def catalog_block(catalog, pattern):
    m = re.search(pattern + r'\s*=\s*\{(.*?)\n\};', catalog, re.S)
    if not m:
        fail('a6xx_catalog.c has no ' + pattern)
    return strip_comments(m.group(1))


def main():
    if '--fetch' in sys.argv[1:]:
        fetch()
    verify()
    r = Resolver()
    catalog = (MSM / 'adreno/a6xx_catalog.c').read_text()
    gpu_c = strip_comments((MSM / 'adreno/a8xx_gpu.c').read_text())

    # The A810 catalog entry: its tables must be the ones this header carries.
    entry = re.search(r'ADRENO_CHIP_IDS\(0x44010000\)(.*?)\n\t\}', catalog, re.S).group(1)
    expected = {'protect': '&a810_protect', 'nonctxt_reglist': 'a810_nonctxt_regs',
                'pwrup_reglist': '&a810_pwrup_reglist', 'dyn_pwrup_reglist': '&a810_dyn_pwrup_reglist',
                'ifpc_reglist': '&a810_ifpc_reglist', 'gbif_cx': 'a840_gbif'}
    for key, value in expected.items():
        if not re.search(r'\.' + key + r'\s*=\s*' + re.escape(value) + r',', entry):
            fail('the A810 entry no longer uses %s = %s' % (key, value))
    facts = {
        'CHIP_ID': 0x44010000,
        'GMEM_BYTES': r.constant(re.search(r'\.gmem\s*=\s*([^,]+),', entry).group(1), {}),
        'GMU_CHIP_ID': int(re.search(r'\.gmu_chipid\s*=\s*(0x[0-9a-fA-F]+)', entry).group(1), 16),
        'MAX_SLICES': int(re.search(r'\.max_slices\s*=\s*(\d+)', entry).group(1)),
        'PREEMPT_RECORD_BYTES': r.constant(re.search(r'\.preempt_record_size\s*=\s*([^,]+),', entry).group(1), {}),
    }
    firmware = {
        'SQE': re.search(r'\[ADRENO_FW_SQE\]\s*=\s*"([^"]+)"', entry).group(1),
        'GMU': re.search(r'\[ADRENO_FW_GMU\]\s*=\s*"([^"]+)"', entry).group(1),
        'ZAP': re.search(r'\.zapfw\s*=\s*"([^"]+)"', entry).group(1),
    }
    if '.gmu_cgc_mode' in entry or '.hwcg' in entry:
        fail('the A810 entry gained gmu_cgc_mode or hwcg; extend this script')

    # Masks and the protect control word from a8xx_gpu.c.
    defines = {}
    for name in MASKS:
        m = re.search(r'#define ' + name + r' \\\n\t\((.*?)\)\n', gpu_c, re.S)
        if not m:
            fail('a8xx_gpu.c has no ' + name)
        defines[name] = r.constant(m.group(1).replace('\\', ' '), defines)
    protect_fn = function_body(gpu_c, 'a8xx_set_cp_protect')
    cntl = r.constant(re.search(r'cntl = (.*?);', protect_fn, re.S).group(1), defines)

    # Every register the gen8 paths name.
    for name in FUNCTIONS:
        body = function_body(gpu_c, name)
        gmu_tokens = set(re.findall(r'gmu_(?:read|write|rmw)\w*\(\s*[^,]+,\s*(REG_\w+(?:\(\d+\))?)', body))
        for token in re.findall(r'\bREG_A[678]XX_[A-Z0-9_]+(?:\(\d+\))?', body):
            r.offset(token)
        for token in re.findall(r'\bA[678]XX_[A-Z0-9_]+', re.sub(r'\bREG_\w+', '', body)):
            if token not in defines:
                r.field(token)
    for token in EXTRA:
        r.offset(token)

    # Bitfields named by those paths, plus the decoder dumps.
    bitfields = {}
    for name in FUNCTIONS:
        body = re.sub(r'\bREG_\w+', '', function_body(gpu_c, name))
        for token in re.findall(r'\bA[678]XX_[A-Z0-9_]+', body):
            if token not in defines:
                out, span, _ = r.field(token)
                bitfields[out] = span
    for name in MASKS:
        body = re.search(r'#define ' + name + r' \\\n\t\((.*?)\)\n', gpu_c, re.S).group(1)
        for token in re.findall(r'\bA[678]XX_[A-Z0-9_]+', body):
            if token not in defines:
                out, span, _ = r.field(token)
                bitfields[out] = span
    for bitset, label in DUMPS:
        for field, span in r.gpu.fields(r.gpu.bitsets[bitset]).items():
            bitfields.setdefault(bitset[5:] + '_' + field, span)

    # Catalog tables.
    def pipe_entries(block):
        rows = []
        for m in re.finditer(r'\{\s*(REG_[A-Z0-9_]+(?:\(\d+\))?)\s*,\s*([^,]+?)\s*,\s*([^}]+?)\s*\}', block):
            rows.append((m.group(1), r.offset(m.group(1)), r.constant(m.group(2), {}), r.constant(m.group(3), {})))
        if len(rows) != block.count('REG_'):
            fail('unparsed pipe-list entry')
        return rows
    nonctxt = pipe_entries(catalog_block(catalog, r'a810_nonctxt_regs\[\]'))
    dyn = pipe_entries(catalog_block(catalog, r'a810_dyn_pwrup_reglist_regs\[\]'))

    def offsets(pattern):
        block = catalog_block(catalog, pattern)
        rows = []
        for item in [i.strip() for i in block.split(',') if i.strip()]:
            m = re.fullmatch(r'(REG_[A-Z0-9_]+(?:\(\d+\))?)(?:\s*\+\s*(\d+))?', item)
            if not m:
                fail('unparsed list entry ' + item)
            rows.append((item, r.offset(m.group(1)) + int(m.group(2) or 0)))
        return rows
    pwrup = offsets(r'a810_pwrup_reglist_regs\[\]')
    ifpc = offsets(r'a810_ifpc_reglist_regs\[\]')

    gbif = []
    block = catalog_block(catalog, r'a840_gbif\[\]')
    for m in re.finditer(r'\{\s*(REG_[A-Z0-9_]+)\s*,\s*(0x[0-9a-fA-F]+)\s*\}', block):
        gbif.append((m.group(1), r.offset(m.group(1)), int(m.group(2), 16)))
    if len(gbif) != block.count('REG_'):
        fail('unparsed a840_gbif entry')

    block = catalog_block(catalog, r'a810_protect_regs\[\]')
    protect = []
    for item in [i.strip() for i in block.split(',\n') if i.strip()]:
        item = item.rstrip(',')
        m = re.fullmatch(r'A6XX_PROTECT_(NORDWR|RDONLY)\((0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+)\)', item)
        if not m:
            fail('unparsed protect entry ' + item)
        reg, length = int(m.group(2), 16), int(m.group(3), 16)
        protect.append(((1 << 31) if m.group(1) == 'NORDWR' else 0) | ((length & 0x3fff) << 18) | (reg & 0x3ffff))
    count_max = int(re.search(r'DECLARE_ADRENO_PROTECT\(a810_protect,\s*(\d+)\)', catalog).group(1))
    if len(protect) > count_max:
        fail('a810_protect is longer than its count_max')
    # a8xx_set_cp_protect: each non-zero entry at CP_PROTECT_GLOBAL(index); the last non-zero one is written again
    # to CP_PROTECT_PIPE(15) of BR and BV, the only place the last-span feature exists.
    protect_writes = [(i, v) for i, v in enumerate(protect) if v]
    final_protect = protect_writes[-1][1]
    if not re.search(r'CP_PROTECT_PIPE\(15\), final_cfg', protect_fn):
        fail('a8xx_set_cp_protect no longer puts the last span at CP_PROTECT_PIPE(15)')

    hw_init = function_body(gpu_c, 'hw_init')
    bicubic = [(int(i), int(v, 16)) for i, v in
               re.findall(r'REG_A8XX_TPL1_BICUBIC_WEIGHTS_TABLE\((\d+)\),\s*(0x[0-9a-fA-F]+|0)\)', hw_init)]
    if [i for i, _ in bicubic] != list(range(len(bicubic))):
        fail('bicubic weights are not one ordered run')
    # Single-value writes of hw_init whose value is a constant (no runtime state).
    values = {}
    for target, value in re.findall(r'(?:gpu|gmu)_write(?:64)?\(\s*[^,]+,\s*(REG_[A-Z0-9_]+)\s*,\s*([^;]+?)\);',
                                    hw_init):
        if 'BICUBIC' in target:
            continue
        try:
            number = r.constant(value, defines)
        except (KeyError, SyntaxError):
            continue
        name = r.used[target][0]
        if values.get(name, number) != number:
            continue
        values[name] = number

    # Header.
    lines = ['/* SPDX-License-Identifier: MIT */', '#ifndef NATIVE_ADRENO_A810_HWINIT_H',
             '#define NATIVE_ADRENO_A810_HWINIT_H', '#include <stdint.h>',
             '/* Generated by scripts/derive-a810-hwinit.py from torvalds/linux %s:' % PIN['commit'][:12],
             ' * registers/adreno a6xx.xml, a6xx_gmu.xml, adreno_common.xml (A8XX variants), adreno/a6xx_catalog.c',
             ' * (the A810 entry) and adreno/a8xx_gpu.c (masks, protect control, bicubic weights). Dword register',
             ' * offsets; GMU registers are a6xx_gmu.xml offsets, not yet relative to the GMU window. Do not edit. */',
             '', '/* A810 catalog facts (the profile must agree). */']
    for key, value in facts.items():
        lines.append('#define A810_%s 0x%xu' % (key, value))
    for key, value in firmware.items():
        lines.append('#define A810_%s_FIRMWARE "%s"' % (key, value))
    lines += ['', '/* adreno_pipe (adreno_common.xml). */']
    for name, value in sorted(r.pipes.items(), key=lambda kv: kv[1]):
        lines.append('#define A810_%s %du' % (name, value))
    lines += ['', '/* GPU registers. */']
    gmu_lines = ['', '/* GMU registers (a6xx_gmu.xml). */']
    for key in sorted(r.used, key=lambda k: (r.used[k][1], k)):
        name, offset, kind, stride, indexed, gmu = r.used[key]
        target = gmu_lines if gmu else lines
        if indexed:
            target.append('#define A810_%s(i) (0x%05xu+(i)*%du)' % (name, offset, stride or 1))
        else:
            target.append('#define A810_%s 0x%05xu%s' % (name, offset, '   /* 64-bit */' if kind == 'reg64' else ''))
    lines += gmu_lines
    lines += ['', '/* Bitfields: one-bit fields as their bit, wider ones as _SHIFT and _MASK. */']
    for name in sorted(bitfields):
        low, high = bitfields[name]
        if low == high:
            lines.append('#define A810_%s (1u<<%d)' % (name, low))
        else:
            lines.append('#define A810_%s_SHIFT %du' % (name, low))
            lines.append('#define A810_%s_MASK 0x%08xu' % (name, ((1 << (high - low + 1)) - 1) << low))
    lines += ['', '/* Masks of a8xx_gpu.c. */']
    for name in MASKS:
        lines.append('#define A810_%s_VALUE 0x%08xu' % (name[5:], defines[name]))
    lines.append('#define A810_CP_PROTECT_CNTL_PIPE_VALUE 0x%08xu' % cntl)
    lines += ['', '/* Constant single-register writes of a8xx hw_init, by register. */']
    for name in sorted(values):
        lines.append('#define A810_INIT_%s 0x%08xu' % (name, values[name]))

    lines += ['', '/* a810_nonctxt_regs: {offset, value, pipe bit mask}; write each once per pipe in its mask, through',
              ' * CP_APERTURE_CNTL_HOST, skipping LPAC. RB_GC_GMEM_PROTECT is held back until after the secure-mode exit. */',
              'static const uint32_t NativeAdrenoA810NonCtxt[][3]={']
    lines += ['    {0x%05x,0x%08x,0x%02x},   /* %s */' % (o, v, p, n[4:]) for n, o, v, p in nonctxt]
    lines += ['};', '#define NATIVE_ADRENO_A810_NONCTXT_COUNT %d' % len(nonctxt), '',
              '/* a810_protect: {CP_PROTECT_GLOBAL index, value}; the last value also goes to CP_PROTECT_PIPE(15) of BR',
              ' * and BV. */', 'static const uint32_t NativeAdrenoA810Protect[][2]={']
    lines += ['    {%2d,0x%08x},' % w for w in protect_writes]
    lines += ['};', '#define NATIVE_ADRENO_A810_PROTECT_COUNT %d' % len(protect_writes),
              '#define NATIVE_ADRENO_A810_PROTECT_COUNT_MAX %d' % count_max,
              '#define NATIVE_ADRENO_A810_PROTECT_LAST 0x%08xu' % final_protect, '',
              '/* a810_ifpc_reglist: IFPC-only pairs, first in the power-up list. */',
              'static const uint32_t NativeAdrenoA810IfpcRegs[]={']
    lines += ['    0x%05x,   /* %s */' % (o, n[4:]) for n, o in ifpc]
    lines += ['};', '#define NATIVE_ADRENO_A810_IFPC_COUNT %d' % len(ifpc), '',
              '/* a810_pwrup_reglist: IFPC and preemption pairs, second. */',
              'static const uint32_t NativeAdrenoA810PwrupRegs[]={']
    lines += ['    0x%05x,   /* %s */' % (o, n[4:]) for n, o in pwrup]
    lines += ['};', '#define NATIVE_ADRENO_A810_PWRUP_COUNT %d' % len(pwrup), '',
              '/* a810_dyn_pwrup_reglist: {offset, pipe bit mask}; one triplet (PIPEID << 12, offset, value read through',
              ' * that pipe on the first slice) per pipe in the mask, pipes BR..DDE_BV in order. */',
              'static const uint32_t NativeAdrenoA810DynPwrup[][2]={']
    lines += ['    {0x%05x,0x%02x},   /* %s */' % (o, p, n[4:]) for n, o, v, p in dyn]
    lines += ['};', '#define NATIVE_ADRENO_A810_DYN_PWRUP_COUNT %d' % len(dyn), '',
              '/* a840_gbif (the A810 entry\'s gbif_cx): {offset, value}. */',
              'static const uint32_t NativeAdrenoA810GbifCx[][2]={']
    lines += ['    {0x%05x,0x%08x},   /* %s */' % (o, v, n[4:]) for n, o, v in gbif]
    lines += ['};', '#define NATIVE_ADRENO_A810_GBIF_CX_COUNT %d' % len(gbif), '',
              '/* TPL1_BICUBIC_WEIGHTS_TABLE(0..%d) in hw_init. */' % (len(bicubic) - 1),
              'static const uint32_t NativeAdrenoA810Bicubic[]={']
    lines += ['    0x%08x,' % v for _, v in bicubic]
    lines += ['};', '#define NATIVE_ADRENO_A810_BICUBIC_COUNT %d' % len(bicubic), '', '#endif']
    text = '\n'.join(lines) + '\n'
    if '--check' in sys.argv[1:]:
        if not OUT.exists() or OUT.read_text() != text:
            fail('NativeAdrenoA810HwInit.h is stale')
        print('PASS: A810 register map and tables match the pinned sources')
        return
    OUT.write_text(text)
    print('PASS: %d registers, %d bitfields, %d non-context, %d protect, %d+%d+%d power-up, %d GBIF, %d bicubic' % (
        len(r.used), len(bitfields), len(nonctxt), len(protect_writes), len(ifpc), len(pwrup), len(dyn), len(gbif),
        len(bicubic)))


main()
