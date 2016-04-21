/******************************************************
** See the accompanying README and LICENSE files
** Author(s): Jeremy Magland
** Created: 3/25/2016
*******************************************************/

#include "fit_stage.h"
#include <QList>
#include "msmisc.h"
#include "diskreadmda.h"
#include <QTime>
#include <math.h>
#include "compute_templates_0.h"
#include "compute_detectability_scores.h"
#include "get_sort_indices.h"
#include "msprefs.h"
#include "omp.h"

double compute_score0(long N, double* X, double* template0);
QList<int> find_events_to_use0(const QList<long>& times, const QList<double>& scores, const fit_stage_opts& opts);
void subtract_template0(long N, double* X, double* template0);
Mda split_into_shells0(const Mda& firings, Define_Shells_Opts opts);
Mda sort_firings_by_time0(const Mda& firings);
QList<long> fit_stage_kernel(Mda& X, QList<long>& times, QList<int>& labels, Mda &templates, const fit_stage_opts& opts);

bool fit_stage_new(const QString& timeseries_path, const QString& firings_path, const QString& firings_out_path, const fit_stage_opts& opts)
{
    QTime timer_total;
    timer_total.start();
    QMap<QString, long> elapsed_times;

    DiskReadMda X(timeseries_path);
    Mda firingsA;
    firingsA.read(firings_path);

    Mda firings = sort_firings_by_time0(firingsA);

    int T = opts.clip_size;
    long L = firings.N2();
    long N = X.N2();
    int M=X.N1();

    Define_Shells_Opts define_shells_opts;
    define_shells_opts.min_shell_size = opts.min_shell_size;
    define_shells_opts.shell_increment = opts.shell_increment;
    Mda firings_split = split_into_shells0(firings, define_shells_opts);

    QList<long> times;
    QList<int> labels;
    for (long i = 0; i < L; i++) {
        times << (long)(firings_split.value(1, i) + 0.5);
        labels << (int)firings_split.value(2, i);
    }

    Mda templates = compute_templates_0(X, firings_split, T); //MxNxK

    long chunk_size = PROCESSING_CHUNK_SIZE;
    long overlap_size = PROCESSING_CHUNK_OVERLAP_SIZE;
    if (N < PROCESSING_CHUNK_SIZE) {
        chunk_size = N;
        overlap_size = 0;
    }

    QList<long> inds_to_use;

    {
        QTime timer_status;
        timer_status.start();
        long num_timepoints_handled = 0;
#pragma omp parallel for
        for (long timepoint = 0; timepoint < N; timepoint += chunk_size) {
            QMap<QString, long> elapsed_times_local;
            Mda chunk;
            QList<long> local_indices;
            QList<long> local_times;
            QList<int> local_labels;
#pragma omp critical(lock1)
            {
                QTime timer;
                timer.start();
                X.readChunk(chunk, 0, timepoint - overlap_size, M, chunk_size + 2 * overlap_size);
                elapsed_times["readChunk"] += timer.elapsed();
                timer.start();
                for (long i = 0; i < L; i++) {
                    if ((timepoint - overlap_size <= times[i]) && (times[i] < timepoint - overlap_size + chunk_size + 2 * overlap_size)) {
                        local_indices << i;
                        local_times << times[i];
                        local_labels << labels[i];
                    }
                }
                elapsed_times["prepare_local_data"] += timer.elapsed();
            }
            QList<long> local_inds_to_use;
            {
                QTime timer;
                timer.start();
                local_inds_to_use = fit_stage_kernel(chunk, local_times, local_labels, templates, opts);
                elapsed_times_local["fit_stage_kernel"] += timer.elapsed();
            }
#pragma omp critical(lock1)
            {
                QTime timer;
                timer.start();
                for (long j = 0; j < local_inds_to_use.count(); j++) {
                    long iii = local_indices[local_inds_to_use[j]];
                    long ttt = local_times[iii];
                    if ((ttt >= overlap_size) && (ttt < overlap_size + chunk_size)) {
                        inds_to_use << iii;
                    }
                }
                elapsed_times_local["set_to_global"] += timer.elapsed();

                num_timepoints_handled += qMin(chunk_size, N - timepoint);
                if ((timer_status.elapsed() > 1000) || (num_timepoints_handled == N) || (timepoint == 0)) {
                    printf("%ld/%ld (%d%%) - Elapsed(s): RC:%g, PLD:%g, KERNEL:%g, STG:%g, Total:%g, %d threads\n",
                           num_timepoints_handled, N,
                           (int)(num_timepoints_handled * 1.0 / N * 100),
                           elapsed_times["readChunk"] * 1.0 / 1000,
                           elapsed_times["prepare_local_data"] * 1.0 / 1000,
                           elapsed_times["fit_stage_kernel"] * 1.0 / 1000,
                           elapsed_times["set_to_global"] * 1.0 / 1000,
                           timer_total.elapsed() * 1.0 / 1000,
                           omp_get_num_threads());
                    timer_status.restart();
                }

            }
        }

        Mda firings_out;
        if (times.count()) {
            printf("using %d/%ld events (%g%%)\n", inds_to_use.count(), (long)times.count(), inds_to_use.count() * 100.0 / times.count());
        }
        firings_out.allocate(firings.N1(), inds_to_use.count());
        for (long i = 0; i < inds_to_use.count(); i++) {
            for (int j = 0; j < firings.N1(); j++) {
                firings_out.set(firings.get(j, inds_to_use[i]), j, i);
            }
        }

        firings_out.write64(firings_out_path);
    }

    return true;
}

//returns the indices to use
QList<long> fit_stage_kernel(Mda& X, QList<long>& times, QList<int>& labels, Mda &templates, const fit_stage_opts& opts)
{
    int T = opts.clip_size;
    int M = X.N1();
    int Tmid = (int)((T + 1) / 2) - 1;
    long L = times.count();

    int K = compute_max(labels);

    QList<double> template_norms;
    template_norms << 0;
    for (int k = 1; k <= K; k++) {
        template_norms << compute_norm(M * T, templates.dataPtr(0, 0, k - 1));
    }

    bool something_changed = true;
    QList<int> all_to_use;
    for (long i = 0; i < L; i++)
        all_to_use << 0;
    int num_passes = 0;
    while (something_changed) {
        num_passes++;
        printf("pass %d... ", num_passes);
        QList<double> scores_to_try;
        QList<long> times_to_try;
        QList<int> labels_to_try;
        QList<long> inds_to_try;
        //QList<double> template_norms_to_try;
        for (long i = 0; i < L; i++) {
            if (all_to_use[i] == 0) {
                long t0 = times[i];
                int k0 = labels[i];
                if (k0 > 0) {
                    double score0 = compute_score0(M * T, X.dataPtr(0, t0 - Tmid), templates.dataPtr(0, 0, k0 - 1));
                    /*
                    if (score0 < template_norms[k0] * template_norms[k0] * 0.5)
                        score0 = 0; //the norm of the improvement needs to be at least 0.5 times the norm of the template
                        */
                    if (score0 > 0) {
                        scores_to_try << score0;
                        times_to_try << t0;
                        labels_to_try << k0;
                        inds_to_try << i;
                    } else {
                        all_to_use[i] = -1; //means we definitely aren't using it (so we will never get here again)
                    }
                }
            }
        }
        QList<int> to_use = find_events_to_use0(times_to_try, scores_to_try, opts);
        something_changed = false;
        long num_added = 0;
        for (long i = 0; i < to_use.count(); i++) {
            if (to_use[i] == 1) {
                something_changed = true;
                num_added++;
                subtract_template0(M * T, X.dataPtr(0, times_to_try[i] - Tmid), templates.dataPtr(0, 0, labels_to_try[i] - 1));
                all_to_use[inds_to_try[i]] = 1;
            }
        }
        printf("added %ld events\n", num_added);
    }

    QList<long> inds_to_use;
    for (long i = 0; i < L; i++) {
        if (all_to_use[i] == 1) {
            inds_to_use << i;
        }
    }

    return inds_to_use;
}

double compute_score0(long N, double* X, double* template0)
{
    Mda resid(1, N);
    double* resid_ptr = resid.dataPtr();
    for (long i = 0; i < N; i++)
        resid_ptr[i] = X[i] - template0[i];
    double norm1 = compute_norm(N, X);
    double norm2 = compute_norm(N, resid_ptr);
    return norm1 * norm1 - norm2 * norm2;
}

QList<int> find_events_to_use0(const QList<long>& times, const QList<double>& scores, const fit_stage_opts& opts)
{
    QList<int> to_use;
    long L = times.count();
    for (long i = 0; i < L; i++)
        to_use << 0;
    double last_best_score = 0;
    long last_best_ind = 0;
    for (long i = 0; i < L; i++) {
        if (scores[i] > 0) {
            if (times[last_best_ind] < times[i] - opts.clip_size) {
                last_best_score = 0; //i think this was a bug.... used to be inside the next for loop!! 4/13/16
                for (int ii = last_best_ind + 1; ii < i; ii++) {
                    if (times[ii] >= times[i] - opts.clip_size) {
                        if (scores[ii] < scores[i])
                            to_use[ii] = 0;
                        if (scores[ii] > last_best_score) {
                            last_best_score = scores[ii];
                            last_best_ind = ii;
                        }
                    }
                }
            }
            if (scores[i] > last_best_score) {
                //to_use[last_best_score]=0; //this was a bug!!!!!!!!!! 4/13/16
                if (last_best_score > 0) {
                    to_use[last_best_ind] = 0;
                }
                to_use[i] = 1;
                last_best_score = scores[i]; //this was another bug, this line was left out! 4/13/16
                last_best_ind = i; //this was another bug, this line was left out! 4/13/16
            }
        }
    }
    return to_use;
}

void subtract_template0(long N, double* X, double* template0)
{
    for (long i = 0; i < N; i++) {
        X[i] -= template0[i];
    }
}
Mda split_into_shells0(const Mda& firings, Define_Shells_Opts opts)
{

    QList<long> labels, labels_new;
    for (long j = 0; j < firings.N2(); j++) {
        labels << (int)firings.value(2, j);
        labels_new << 0;
    }
    int K = compute_max(labels);
    int k2 = 1;
    for (int k = 1; k <= K; k++) {
        QList<long> inds_k = find_label_inds(labels, k);
        QList<double> peaks;
        for (long j = 0; j < inds_k.count(); j++) {
            peaks << firings.value(3, inds_k[j]);
        }
        QList<Shell> shells = define_shells(peaks, opts);
        for (int a = 0; a < shells.count(); a++) {
            QList<long> s_inds = shells[a].inds;
            for (long b = 0; b < s_inds.count(); b++) {
                labels[inds_k[s_inds[b]]] = k2;
            }
            k2++;
        }
    }

    Mda firings_ret = firings;
    for (long j = 0; j < firings.N2(); j++) {
        firings_ret.setValue(labels[j], 2, j);
    }
    return firings_ret;
}

Mda sort_firings_by_time0(const Mda& firings)
{
    QList<double> times;
    for (long i = 0; i < firings.N2(); i++) {
        times << firings.value(1, i);
    }
    QList<long> sort_inds = get_sort_indices(times);

    Mda F(firings.N1(), firings.N2());
    for (long i = 0; i < firings.N2(); i++) {
        for (int j = 0; j < firings.N1(); j++) {
            F.setValue(firings.value(j, sort_inds[i]), j, i);
        }
    }

    return F;
}