import { useRef, type DependencyList } from 'react';

// 简单的深比较函数
function deepEqual(obj1: any, obj2: any): boolean {
    if (obj1 === obj2) return true;

    if (obj1 == null || obj2 == null) return false;

    if (typeof obj1 !== 'object' || typeof obj2 !== 'object') {
        return obj1 === obj2;
    }

    const keys1 = Object.keys(obj1);
    const keys2 = Object.keys(obj2);

    if (keys1.length !== keys2.length) return false;

    for (let key of keys1) {
        if (!keys2.includes(key)) return false;
        if (!deepEqual(obj1[key], obj2[key])) return false;
    }

    return true;
}

// 使用深比较的自定义memo hook
export function useDeepCompareMemo<T>(
    factory: () => T,
    deps: DependencyList
): T {
    const ref = useRef<{ deps: DependencyList; value: T } | undefined>(undefined);

    if (!ref.current || !deepEqual(deps, ref.current.deps)) {
        ref.current = { deps, value: factory() };
    }

    return ref.current.value;
}

// 自定义hook来比较对象是否真正改变
export function useShallowCompareMemo<T>(factory: () => T, deps: any[]): T {
    const ref = useRef<{ deps: any[]; value: T } | undefined>(undefined);

    const hasChanged = !ref.current ||
        ref.current.deps.length !== deps.length ||
        deps.some((dep, index) => ref.current!.deps[index] !== dep);

    if (hasChanged) {
        ref.current = { deps, value: factory() };
    }

    return ref.current!.value;
}
