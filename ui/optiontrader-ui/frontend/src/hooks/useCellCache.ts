import { useState, useCallback, useRef } from 'react';

export interface CellCacheEntry {
  value: any;
  dirty: boolean;
  lastUpdate: number; // 添加时间戳以支持更精确的更新控制
}

export interface CellCache {
  [row: number]: {
    [col: string]: {
      [role: string]: CellCacheEntry;
    }
  }
}

export const useCellCache = () => {
  const [cellCache, setCellCache] = useState<CellCache>({});
  const cellCacheRef = useRef(cellCache);
  
  // 更新引用
  cellCacheRef.current = cellCache;

  const isCellDirty = useCallback((row: number, col: string, role: string = 'default'): boolean => {
    const rowCache = cellCacheRef.current[row];
    if (!rowCache) return true; // 如果没有缓存，认为是脏数据

    const colCache = rowCache[col];
    if (!colCache) return true; // 如果没有列缓存，认为是脏数据

    const roleCache = colCache[role];
    if (!roleCache) return true; // 如果没有角色缓存，认为是脏数据

    return roleCache.dirty;
  }, []);

  const getCellRoleValue = useCallback((row: number, col: string, role: string = 'default'): any => {
    const rowCache = cellCacheRef.current[row];
    if (!rowCache) return undefined;

    const colCache = rowCache[col];
    if (!colCache) return undefined;

    const roleCache = colCache[role];
    if (!roleCache) return undefined;

    return roleCache.value;
  }, []);

  const setCellRoleValue = useCallback((row: number, col: string, role: string = 'default', value: any) => {
    setCellCache(prev => {
      const newRowCache = {
        ...prev[row],
        [col]: {
          ...prev[row]?.[col],
          [role]: {
            value,
            dirty: false,
            lastUpdate: Date.now()
          }
        }
      };

      return {
        ...prev,
        [row]: newRowCache
      };
    });
  }, []);

  const setCellDirty = useCallback((row: number, col: string, role: string = 'default', dirty: boolean = true) => {
    setCellCache(prev => {
      if (!prev[row]?.[col]?.[role]) {
        // 如果尚未存在缓存，创建一个空的脏状态
        return {
          ...prev,
          [row]: {
            ...prev[row],
            [col]: {
              ...prev[row]?.[col],
              [role]: {
                value: undefined,
                dirty,
                lastUpdate: Date.now()
              }
            }
          }
        };
      }

      const newRowCache = {
        ...prev[row],
        [col]: {
          ...prev[row]?.[col],
          [role]: {
            ...prev[row]?.[col]?.[role],
            dirty,
            lastUpdate: Date.now()
          }
        }
      };

      return {
        ...prev,
        [row]: newRowCache
      };
    });
  }, []);

  // 新增方法：批量设置单元格脏状态
  const setCellsDirty = useCallback((changes: Array<{row: number, col: string, role: string}>) => {
    setCellCache(prev => {
      const newData = { ...prev };
      
      changes.forEach(({row, col, role}) => {
        if (!newData[row]) {
          newData[row] = {};
        }
        if (!newData[row][col]) {
          newData[row][col] = {};
        }
        
        if (!newData[row][col][role]) {
          newData[row][col][role] = {
            value: undefined,
            dirty: true,
            lastUpdate: Date.now()
          };
        } else {
          newData[row][col][role].dirty = true;
          newData[row][col][role].lastUpdate = Date.now();
        }
      });
      
      return newData;
    });
  }, []);

  const updateCellData = useCallback((row: number, col: string, data: Record<string, any>) => {
    setCellCache(prev => {
      const newData = { ...prev };
      
      if (!newData[row]) {
        newData[row] = {};
      }
      
      if (!newData[row][col]) {
        newData[row][col] = {};
      }
      
      // 只更新发生变化的字段，避免覆盖未变化的数据
      Object.entries(data).forEach(([role, value]) => {
        // 检查当前值是否发生变化
        const currentValue = newData[row]?.[col]?.[role]?.value;
        if (JSON.stringify(currentValue) !== JSON.stringify(value)) {
          newData[row][col][role] = {
            value,
            dirty: true, // 新数据标记为脏，需要更新显示
            lastUpdate: Date.now()
          };
        } else {
          // 如果值没有变化，保持原有状态但更新时间戳
          if (newData[row][col][role]) {
            newData[row][col][role].lastUpdate = Date.now();
          } else {
            // 如果之前不存在该字段，添加它并标记为脏
            newData[row][col][role] = {
              value,
              dirty: true,
              lastUpdate: Date.now()
            };
          }
        }
      });

      return newData;
    });
  }, []);

  const clearRowCache = useCallback((row: number) => {
    setCellCache(prev => {
      const newData = { ...prev };
      delete newData[row];
      return newData;
    });
  }, []);

  const clearCache = useCallback(() => {
    setCellCache({});
  }, []);

  // 新增方法：根据过期时间清理缓存
  const cleanupCache = useCallback((maxAge: number = 300000) => { // 默认5分钟
    const now = Date.now();
    setCellCache(prev => {
      const newData = { ...prev };
      
      for (const row in newData) {
        for (const col in newData[row]) {
          for (const role in newData[row][col]) {
            if (now - newData[row][col][role].lastUpdate > maxAge) {
              delete newData[row][col][role];
            }
          }
          
          // 如果列下没有角色了，删除整列
          if (Object.keys(newData[row][col]).length === 0) {
            delete newData[row][col];
          }
        }
        
        // 如果行下没有列了，删除整行
        if (Object.keys(newData[row]).length === 0) {
          delete newData[row];
        }
      }
      
      return newData;
    });
  }, []);

  return {
    isCellDirty,
    getCellRoleValue,
    setCellRoleValue,
    setCellDirty,
    setCellsDirty,
    updateCellData,
    clearRowCache,
    clearCache,
    cleanupCache
  };
};
