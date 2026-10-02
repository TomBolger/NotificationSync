package com.matejdro.bucketsync.background

import android.content.Context
import androidx.work.CoroutineWorker
import androidx.work.ListenableWorker
import androidx.work.WorkerParameters
import com.matejdro.bucketsync.BucketSyncWatchappOpenController
import com.matejdro.bucketsync.di.BucketSyncWorkerKey
import com.matejdro.pebble.bluetooth.common.WatchappId
import dev.zacsweers.metro.AppScope
import dev.zacsweers.metro.Assisted
import dev.zacsweers.metro.AssistedFactory
import dev.zacsweers.metro.AssistedInject
import dev.zacsweers.metro.ContributesIntoMap
import dev.zacsweers.metro.Inject
import dev.zacsweers.metro.binding
import io.rebble.pebblekit2.client.PebbleInfoRetriever
import io.rebble.pebblekit2.client.PebbleSender
import io.rebble.pebblekit2.common.model.WatchIdentifier
import io.rebble.pebblekit2.model.Watchapp
import kotlinx.coroutines.flow.first
import logcat.logcat
import si.inova.kotlinova.core.reporting.ErrorReporter
import java.util.UUID

@AssistedInject
class OpenWatchappWorker(
   context: Context,
   private val workerController: WorkControllerImpl,
   private val pebbleSender: PebbleSender,
   private val errorReporter: ErrorReporter,
   private val autoSyncNotifier: BucketSyncWatchappOpenController,
   private val pebbleInfoRetriever: PebbleInfoRetriever,
   @WatchappId
   private val watchappId: UUID,
   @Assisted
   private val params: WorkerParameters,
) : CoroutineWorker(context, params) {
   override suspend fun doWork(): Result {
      val watchId = params.inputData.getString(DATA_KEY_WATCH)?.let { WatchIdentifier(it) }
      if (watchId == null) {
         errorReporter.report(Exception("Got missing watch ID"))
         return Result.failure()
      }

      // The foreground app may have changed since this work was scheduled. Never yank the user out of an app
      // just to sync in the background; try again later instead.
      val currentWatchapp = pebbleInfoRetriever.getActiveApp(watchId).first()
      if (currentWatchapp?.type != Watchapp.Type.WATCHFACE) {
         logcat { "Foreground app ${currentWatchapp ?: "null"} on $watchId is not a watchface. Retrying later." }
         return Result.retry()
      }

      logcat { "Opening the watchapp on the $watchId" }
      autoSyncNotifier.setNextWatchappOpenForAutoSync()
      pebbleSender.startAppOnTheWatch(watchappId)
      workerController.cancelForegroundAppWorker(watchId)
      return Result.success()
   }

   @AssistedFactory
   @Inject
   @ContributesIntoMap(AppScope::class, binding<(WorkerParameters) -> ListenableWorker>())
   @BucketSyncWorkerKey(OpenWatchappWorker::class)
   interface Factory : (WorkerParameters) -> ListenableWorker {
      fun create(params: WorkerParameters): OpenWatchappWorker

      override fun invoke(params: WorkerParameters): ListenableWorker {
         return create(params)
      }
   }

   companion object {
      const val DATA_KEY_WATCH = "Watch"
   }
}
