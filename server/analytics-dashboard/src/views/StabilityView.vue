<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Stability</h2>
      </div>

      <div v-if="loading" class="loading">Loading...</div>
      <div v-else-if="error" class="error">{{ error }}</div>
      <template v-else-if="data">
        <div class="metrics-row">
          <MetricCard
            title="Crash Rate"
            :value="`${overallCrashRate.toFixed(1)}%`"
            subtitle="of sessions"
            color="var(--accent-red)"
          />
          <MetricCard
            title="Avg Uptime Before Crash"
            :value="formatDuration(data.avg_uptime_sec)"
            subtitle="before crash"
            color="var(--accent-yellow)"
          />
          <MetricCard
            title="Memory Warnings"
            :value="totalMemoryWarnings.toLocaleString()"
            subtitle="in period"
            color="#f59e0b"
          />
          <MetricCard
            title="Klippy Errors"
            :value="totalKlippyErrors.toLocaleString()"
            subtitle="in period"
            color="#f97316"
          />
        </div>

        <div class="chart-section">
          <h3>Crash Rate Over Time</h3>
          <LineChart :data="crashRateChartData" :options="crashRateOpts" />
        </div>

        <div class="grid-2col">
          <div class="chart-section">
            <h3>Crash Rate by Version</h3>
            <BarChart :data="versionChartData" :options="barPercentOpts" />
          </div>
          <div class="chart-section">
            <h3>Crashes by Signal</h3>
            <PieChart :data="signalChartData" />
          </div>
        </div>

        <div class="grid-2col">
          <div class="chart-section">
            <h3>Klippy Errors &amp; Shutdowns</h3>
            <LineChart :data="klippyChartData" />
          </div>
          <div class="chart-section">
            <h3>Memory Warnings Over Time</h3>
            <LineChart :data="memoryWarningsChartData" />
          </div>
        </div>

        <div class="chart-section">
          <h3>Error Hotspots</h3>
          <BarChart :data="errorCategoryChartData" :options="horizontalOpts" />
        </div>

        <div class="chart-section">
          <h3>Error Codes</h3>
          <div v-if="data.error_codes.length === 0" class="empty-state">No error codes found in this period.</div>
          <div v-else class="table-wrapper">
            <table class="data-table">
              <thead>
                <tr>
                  <th>Category</th>
                  <th>Code</th>
                  <th>Count</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="(row, i) in data.error_codes" :key="i">
                  <td><span class="badge category">{{ row.category }}</span></td>
                  <td class="mono">{{ row.code }}</td>
                  <td class="mono">{{ row.count.toLocaleString() }}</td>
                </tr>
              </tbody>
            </table>
          </div>
        </div>

        <div class="chart-section">
          <h3>Recent Crashes</h3>
          <div v-if="data.recent_crashes.length === 0" class="empty-state">No crash events found in this period.</div>
          <div v-else class="table-wrapper">
            <table class="crash-table">
              <thead>
                <tr>
                  <th>Time</th>
                  <th>Version</th>
                  <th>Signal</th>
                  <th>Platform</th>
                  <th>Uptime</th>
                  <th>Device</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="(crash, i) in data.recent_crashes" :key="i">
                  <td class="mono">{{ formatTimestamp(crash.timestamp) }}</td>
                  <td><span class="badge version">{{ crash.version || '\u2014' }}</span></td>
                  <td><span class="badge" :class="signalClass(crash.signal)">{{ crash.signal || '\u2014' }}</span></td>
                  <td>{{ crash.platform || '\u2014' }}</td>
                  <td class="mono">{{ formatDuration(crash.uptime_sec) }}</td>
                  <td class="mono device-id">{{ shortDeviceId(crash.device_id) }}</td>
                </tr>
              </tbody>
            </table>
          </div>
        </div>
      </template>
    </div>
  </AppLayout>
</template>

<script setup lang="ts">
import { ref, watch, computed } from 'vue'
import AppLayout from '@/components/AppLayout.vue'
import MetricCard from '@/components/MetricCard.vue'
import LineChart from '@/components/LineChart.vue'
import BarChart from '@/components/BarChart.vue'
import PieChart from '@/components/PieChart.vue'
import { useFiltersStore } from '@/stores/filters'
import { api } from '@/services/api'
import type { StabilityData } from '@/services/api'
import type { ChartOptions } from 'chart.js'

const COLORS = ['#3b82f6', '#10b981', '#f59e0b', '#ef4444', '#8b5cf6', '#ec4899', '#06b6d4', '#84cc16']

const filters = useFiltersStore()
const data = ref<StabilityData | null>(null)
const loading = ref(true)
const error = ref('')

function formatDuration(seconds: number): string {
  if (!seconds) return '0m'
  const h = Math.floor(seconds / 3600)
  const m = Math.floor((seconds % 3600) / 60)
  return h > 0 ? `${h}h ${m}m` : `${m}m`
}

function formatTimestamp(ts: string): string {
  try {
    const d = new Date(ts)
    return d.toLocaleDateString('en-US', { month: 'short', day: 'numeric' }) +
      ' ' + d.toLocaleTimeString('en-US', { hour: '2-digit', minute: '2-digit', hour12: false })
  } catch {
    return ts
  }
}

function shortDeviceId(id: string): string {
  if (!id) return '\u2014'
  return id.slice(0, 8) + '...'
}

function signalClass(signal: string): string {
  if (signal === 'SIGSEGV') return 'signal-segv'
  if (signal === 'SIGABRT') return 'signal-abrt'
  return 'signal-other'
}

const overallCrashRate = computed(() => {
  const trend = data.value?.crash_rate_trend ?? []
  const crashes = trend.reduce((sum, d) => sum + d.crashes, 0)
  const sessions = trend.reduce((sum, d) => sum + d.sessions, 0)
  return sessions > 0 ? (crashes / sessions) * 100 : 0
})

const totalMemoryWarnings = computed(() =>
  (data.value?.memory_warnings_trend ?? []).reduce((sum, d) => sum + d.count, 0)
)

const totalKlippyErrors = computed(() =>
  (data.value?.klippy_trend ?? []).reduce((sum, d) => sum + d.errors, 0)
)

const crashRateOpts: ChartOptions<'line'> = {
  scales: {
    y: {
      ticks: {
        callback: (value) => `${value}%`,
        color: '#94a3b8',
      },
      grid: { color: 'rgba(45, 51, 72, 0.5)' },
    },
    x: {
      ticks: { color: '#94a3b8' },
      grid: { color: 'rgba(45, 51, 72, 0.5)' },
    },
  },
  plugins: {
    tooltip: {
      callbacks: {
        label: (ctx) => {
          const idx = ctx.dataIndex
          const trend = data.value?.crash_rate_trend ?? []
          const point = trend[idx]
          if (point) {
            return `Rate: ${(point.rate * 100).toFixed(2)}% (${point.crashes} crashes / ${point.sessions} sessions)`
          }
          return `${ctx.formattedValue}%`
        }
      }
    }
  }
}

const barPercentOpts: ChartOptions<'bar'> = {
  scales: {
    y: {
      ticks: {
        callback: (value) => `${value}%`,
        color: '#94a3b8',
      },
      grid: { color: 'rgba(45, 51, 72, 0.5)' },
    },
    x: {
      ticks: { color: '#94a3b8' },
      grid: { color: 'rgba(45, 51, 72, 0.5)' },
    },
  },
}

const horizontalOpts: ChartOptions<'bar'> = { indexAxis: 'y', scales: { y: { ticks: { autoSkip: false } } } }

const crashRateChartData = computed(() => ({
  labels: data.value?.crash_rate_trend.map(d => d.date) ?? [],
  datasets: [{
    label: 'Crash Rate %',
    data: data.value?.crash_rate_trend.map(d => d.rate * 100) ?? [],
    borderColor: '#ef4444',
    backgroundColor: 'rgba(239, 68, 68, 0.1)',
    fill: true,
    tension: 0.3
  }]
}))

function compareVersions(a: string, b: string): number {
  const pa = a.replace(/^v/, '').split('.').map(Number)
  const pb = b.replace(/^v/, '').split('.').map(Number)
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const diff = (pa[i] ?? 0) - (pb[i] ?? 0)
    if (diff !== 0) return diff
  }
  return 0
}

const versionChartData = computed(() => {
  const sorted = [...(data.value?.by_version ?? [])].sort((a, b) => compareVersions(a.version, b.version))
  return {
    labels: sorted.map(v => v.version),
    datasets: [{
      label: 'Crash Rate %',
      data: sorted.map(v => v.rate * 100),
      backgroundColor: '#ef4444'
    }]
  }
})

const signalChartData = computed(() => ({
  labels: data.value?.by_signal.map(s => s.signal) ?? [],
  datasets: [{
    data: data.value?.by_signal.map(s => s.count) ?? [],
    backgroundColor: COLORS
  }]
}))

const klippyChartData = computed(() => ({
  labels: data.value?.klippy_trend.map(d => d.date) ?? [],
  datasets: [
    {
      label: 'Errors',
      data: data.value?.klippy_trend.map(d => d.errors) ?? [],
      borderColor: '#f97316',
      backgroundColor: 'rgba(249, 115, 22, 0.1)',
      fill: false,
      tension: 0.3
    },
    {
      label: 'Shutdowns',
      data: data.value?.klippy_trend.map(d => d.shutdowns) ?? [],
      borderColor: '#ef4444',
      backgroundColor: 'rgba(239, 68, 68, 0.1)',
      fill: false,
      tension: 0.3
    }
  ]
}))

const memoryWarningsChartData = computed(() => ({
  labels: data.value?.memory_warnings_trend.map(d => d.date) ?? [],
  datasets: [{
    label: 'Warnings',
    data: data.value?.memory_warnings_trend.map(d => d.count) ?? [],
    borderColor: '#f59e0b',
    backgroundColor: 'rgba(245, 158, 11, 0.1)',
    fill: true,
    tension: 0.3
  }]
}))

const errorCategoryChartData = computed(() => ({
  labels: data.value?.error_categories.map(c => c.category) ?? [],
  datasets: [{
    label: 'Count',
    data: data.value?.error_categories.map(c => c.count) ?? [],
    backgroundColor: '#ef4444'
  }]
}))

async function fetchData() {
  loading.value = true
  error.value = ''
  try {
    data.value = await api.getStability(filters.queryString)
  } catch (e) {
    error.value = e instanceof Error ? e.message : 'Failed to load data'
  } finally {
    loading.value = false
  }
}

watch(() => filters.queryString, fetchData, { immediate: true })
</script>

<style scoped>
.page-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 24px;
}

.page-header h2 {
  font-size: 20px;
  font-weight: 600;
}

.metrics-row {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 16px;
  margin-bottom: 24px;
}

.grid-2col {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 16px;
  margin-bottom: 24px;
  align-items: start;
}

.chart-section {
  margin-bottom: 24px;
}

.chart-section h3 {
  font-size: 14px;
  font-weight: 500;
  color: var(--text-secondary);
  margin-bottom: 12px;
}

.loading, .error, .empty-state {
  padding: 40px;
  text-align: center;
  color: var(--text-secondary);
}

.error {
  color: var(--accent-red);
}

.empty-state {
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: 8px;
}

.table-wrapper {
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: 8px;
  overflow-x: auto;
}

.data-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
}

.data-table th {
  text-align: left;
  padding: 10px 14px;
  font-weight: 500;
  color: var(--text-secondary);
  border-bottom: 1px solid var(--border);
  white-space: nowrap;
}

.data-table td {
  padding: 8px 14px;
  border-bottom: 1px solid var(--border);
  color: var(--text-primary);
  white-space: nowrap;
}

.data-table tbody tr:last-child td {
  border-bottom: none;
}

.data-table tbody tr:hover {
  background: rgba(255, 255, 255, 0.03);
}

.crash-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
}

.crash-table th {
  text-align: left;
  padding: 10px 14px;
  font-weight: 500;
  color: var(--text-secondary);
  border-bottom: 1px solid var(--border);
  white-space: nowrap;
}

.crash-table td {
  padding: 8px 14px;
  border-bottom: 1px solid var(--border);
  color: var(--text-primary);
  white-space: nowrap;
}

.crash-table tbody tr:last-child td {
  border-bottom: none;
}

.crash-table tbody tr:hover {
  background: rgba(255, 255, 255, 0.03);
}

.mono {
  font-family: 'SF Mono', 'Fira Code', monospace;
  font-size: 12px;
}

.device-id {
  color: var(--text-secondary);
}

.badge {
  display: inline-block;
  padding: 2px 8px;
  border-radius: 4px;
  font-size: 12px;
  font-weight: 500;
}

.badge.version {
  background: rgba(59, 130, 246, 0.15);
  color: #60a5fa;
}

.badge.category {
  background: rgba(139, 92, 246, 0.15);
  color: #a78bfa;
}

.badge.signal-segv {
  background: rgba(239, 68, 68, 0.15);
  color: #f87171;
}

.badge.signal-abrt {
  background: rgba(245, 158, 11, 0.15);
  color: #fbbf24;
}

.badge.signal-other {
  background: rgba(139, 92, 246, 0.15);
  color: #a78bfa;
}
</style>
