<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Performance</h2>
      </div>

      <div v-if="loading" class="loading">Loading...</div>
      <div v-else-if="error" class="error">{{ error }}</div>
      <template v-else-if="data">
        <div class="metrics-row">
          <MetricCard
            title="Median Frame Time"
            :value="`${data.fleet_p50_ms}ms`"
            color="var(--accent-blue)"
          />
          <MetricCard
            title="Fleet Drop Rate"
            :value="`${(data.fleet_drop_rate * 100).toFixed(2)}%`"
            color="var(--accent-yellow)"
          />
          <MetricCard
            title="High Drop Devices (>5%)"
            :value="`${data.high_drop_devices} / ${data.total_devices}`"
            color="var(--accent-red)"
          />
          <MetricCard
            title="Worst Panel"
            :value="data.worst_panel || 'N/A'"
            color="var(--accent-purple)"
          />
        </div>

        <div class="chart-section">
          <h3>Frame Time Trends (ms)</h3>
          <LineChart :data="frameTimeTrendData" />
        </div>

        <div class="grid-2col">
          <div class="chart-section">
            <h3>Drop Rate by Platform</h3>
            <BarChart :data="dropByPlatformData" :options="horizontalBarOpts" />
          </div>
          <div class="chart-section">
            <h3>Jankiest Panels (by p95 frame time)</h3>
            <BarChart :data="jankiestPanelsData" :options="horizontalBarOpts" />
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
import { useFiltersStore } from '@/stores/filters'
import { api } from '@/services/api'
import type { PerformanceData } from '@/services/api'
import { horizontalBarOpts } from '@/utils/chart'

const filters = useFiltersStore()
const data = ref<PerformanceData | null>(null)
const loading = ref(true)
const error = ref('')

const frameTimeTrendData = computed(() => ({
  labels: data.value?.frame_time_trend.map(d => d.date) ?? [],
  datasets: [
    {
      label: 'p50',
      data: data.value?.frame_time_trend.map(d => d.p50) ?? [],
      borderColor: '#10b981',
      fill: false,
      tension: 0.3,
    },
    {
      label: 'p95',
      data: data.value?.frame_time_trend.map(d => d.p95) ?? [],
      borderColor: '#f59e0b',
      fill: false,
      tension: 0.3,
    },
    {
      label: 'p99',
      data: data.value?.frame_time_trend.map(d => d.p99) ?? [],
      borderColor: '#ef4444',
      fill: false,
      tension: 0.3,
    },
  ],
}))

const dropByPlatformData = computed(() => ({
  labels: data.value?.drop_rate_by_platform.map(p => p.platform) ?? [],
  datasets: [{
    label: 'Drop Rate %',
    data: data.value?.drop_rate_by_platform.map(p => +(p.rate * 100).toFixed(2)) ?? [],
    backgroundColor: '#ef4444',
  }],
}))

const jankiestPanelsData = computed(() => {
  const sorted = [...(data.value?.jankiest_panels ?? [])].sort((a, b) => b.avg_p95_ms - a.avg_p95_ms)
  return {
    labels: sorted.map(p => `${p.panel} (${p.avg_p95_ms}ms)`),
    datasets: [{
      label: 'Avg p95 (ms)',
      data: sorted.map(p => p.avg_p95_ms),
      backgroundColor: '#8b5cf6',
    }],
  }
})

async function fetchData() {
  loading.value = true
  error.value = ''
  try {
    data.value = await api.getPerformance(filters.queryString)
  } catch (e) {
    error.value = e instanceof Error ? e.message : 'Failed to load data'
  } finally {
    loading.value = false
  }
}

watch(() => filters.queryString, fetchData, { immediate: true })
</script>

<style scoped>
.page-header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 24px; }
.page-header h2 { font-size: 20px; font-weight: 600; }
.metrics-row { display: grid; grid-template-columns: repeat(4, 1fr); gap: 16px; margin-bottom: 24px; }
.grid-2col { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; margin-bottom: 24px; }
.chart-section { margin-bottom: 24px; }
.chart-section h3 { font-size: 14px; font-weight: 500; color: var(--text-secondary); margin-bottom: 12px; }
.loading, .error { padding: 40px; text-align: center; color: var(--text-secondary); }
.error { color: var(--accent-red); }
</style>
