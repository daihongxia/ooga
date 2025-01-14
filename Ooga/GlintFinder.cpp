#include "GlintFinder.h"

//#define DEBUGFILE

#define N_LEDS 6
#define N_GLINT_CANDIDATES 2

// TODO: Give N_glint_candidates as input parameter! t: Miika

#ifdef DEBUGFILE

#include <iostream>
#include <fstream>

static std::ofstream dbgf("debug_stream.txt");

#endif

static const bool DO_PERF_T = false;

cv::Mat log_mvnpdf(cv::Mat x, cv::Mat mu, cv::Mat C) {
	// Computes the logarithm of multivariate normal probability density function (mvnpdf) of x with
	// mean mu, covariance C, and log'd normalization factor log_norm_factor, using Cholesky decomposition.
	// The matrix sizes must be [D x N] where D is the dimension and N is the number of points in which to evalute this function.
	// Give the arguments as CV_32F.
	// Note! There's no check for the validity of the given covariance (such as Matlab's cholCov); hence, this may return NaN's.

	float log_norm_factor = -0.5*(2 * log(2 * PI) + log(determinant(C)));
	cv::Mat invChol = C.clone();
	cv::Cholesky(invChol.ptr<float>(), invChol.step, invChol.cols, 0, 0, 0);
	// Opencv's 'Cholesky' returns a bit weird matrix... it's almost the inverse of real cholesky but the off-diagonals must be adjusted:
	invChol.at<float>(0, 1) = -invChol.at<float>(0, 0)*invChol.at<float>(1, 1)*invChol.at<float>(1, 0);
	invChol.at<float>(1, 0) = 0;

	cv::Mat x_mu(mu.rows, mu.cols, CV_32F);
	x_mu = x - mu;

	cv::Mat mahal2(x.cols, x.rows, CV_32F);
	cv::Mat mahal1(x.cols, x.rows, CV_32F);
	mahal1 = x_mu.t() * invChol;
	//pow(x_mu.t() * invChol, 2, mahal2);
	cv::pow(mahal1, 2, mahal2);
	cv::Mat mahal2_sum(x.cols, 1, CV_32F);
	cv::reduce(mahal2, mahal2_sum, 1, CV_REDUCE_SUM);

	return -0.5*mahal2_sum.t() + log_norm_factor;
}

//trying for a faster logmvnpdf?
//http://gallery.rcpp.org/articles/dmvnorm_arma/


GlintFinder::GlintFinder()
{

	pt = new TPerformanceTimer();

	framecounter = 0;
	float scale = 40.0f;   // TODO: read these from files (left and right scale separately)
}


GlintFinder::~GlintFinder()
{
	if (pt != nullptr) {
		delete pt;
	}
#ifdef DEBUGFILE
	dbgf.close();
#endif
}

void GlintFinder::Initialize(cv::Mat initCM, float initMU_X[], float initMU_Y[], int muSize)
{
	int cols = 640;
	int rows = 480;
	this->setCropWindowSize(150, 100, 350, 350);

	for (int i = 0; i < muSize; ++i) {
		MU_X[i] = initMU_X[i];
		MU_Y[i] = initMU_Y[i];
	}

	initCM.copyTo(this->CM);
	//CM = cv::Mat(12, 12, CV_32F);
	//CM.setTo(0.5);
}

void GlintFinder::setCropWindowSize(int xmin, int ymin, int width, int height)
{
	this->cropminX = xmin;
	this->cropminY = ymin;
	this->cropsizeX = width;
	this->cropsizeY = height;

	//init matrices
	eyeImage_filtered = cv::UMat(cropsizeX, cropsizeY, CV_8UC1);
	eyeImage_filtered_clone = cv::UMat(cropsizeX, cropsizeY, CV_8UC1);
	eyeImage_aux = cv::UMat(cropsizeX, cropsizeY, CV_8UC1);
	//cv::UMat eyeImage_cropped;
	//cv::UMat eyeImage_aux_crop;
}

std::vector<cv::Point2d> GlintFinder::getGlints(cv::UMat eyeImage_diff, cv::Point2d pupil_center, std::vector<cv::Point2d> glintPoints_prev, float theta,
	cv::Mat glint_kernel, double &score, float loglhoods[], float glint_beta, float glint_reg_coef,
	int N_glint_candidates, bool bUpdateGlintModel)

{


	if (DO_PERF_T) pt->start();

	// Set parameters:
	int Svert = eyeImage_diff.rows;   // Image size, vertical
	int Shori = eyeImage_diff.cols;   // Image size, horizontal
	int N_leds = 6;     // Number of leds, TODO: define elsewhere

	//N_glint_candidates = 6;   // TODO: Number of glint candidates (as input argument!)

	float delta_x = 100;  // zoom area (horizontal) around pupil center where to search the glints (was 100)
	float delta_y = 75;  // zoom area (vertical) around pupil center where to search the glints (was 75)
	int delta_glint = 10;  // In searching for glint candidates, insert this amount of zero pixels around each found candidate
	float delta_extra = 7;   // Some extra for cropping the area where the likelihood will be evaluated (was 10)

	//float beta = 100;       // the likelihood parameter (an input argument nowadays)
	//float reg_coef = 1;    // initial regularization coefficient for the covariance matrix (if zero, might result in non-valid covariance matrix ---> crash) (an input argument nowadays)
	float beta = glint_beta;
	float reg_coef = glint_reg_coef;

	N_glint_candidates = N_GLINT_CANDIDATES;  // todo: this should be an input argument!!

	// Initialize variables
	int N_particles = N_leds * N_glint_candidates;

	//replaced these with defined constants for now
	float glint_candidates[2][N_GLINT_CANDIDATES];
	float glints_x[N_GLINT_CANDIDATES*N_LEDS][N_LEDS] = { 0 }; //init with zeros, simplifies things around line 246
	float glints_y[N_GLINT_CANDIDATES*N_LEDS][N_LEDS] = { 0 };
	float loglkh_at_candidates[N_GLINT_CANDIDATES] = { 0 };
	float lhoods[N_GLINT_CANDIDATES*N_LEDS][N_LEDS] = { 0 };
	float maps[N_GLINT_CANDIDATES*N_LEDS][N_LEDS] = { 0 };
	int ORDERS[N_LEDS][N_LEDS] = { 0 };

	double min_val, max_val;
	cv::Point min_loc, max_loc;
	float im_max;
	float x_prev;
	float y_prev;
	cv::Mat mu_cond(2, 1, CV_32F);
	cv::Mat mu_dyn(2, 1, CV_32F);
	cv::Mat mu_dum(2, 1, CV_32F);
	cv::Mat C_cond; //(2, 2, CV_32F);
	cv::Mat C_dyn = cv::Mat::eye(2, 2, CV_32F)*std::abs(theta) * 100; // (loppukerroin oli 100)
	cv::Mat invC_dyn(2, 2, CV_32F);
	if (cv::invert(C_dyn, invC_dyn, cv::DECOMP_CHOLESKY) == 0) {
		printf("getGlints.cpp: C_dynamical could not be inverted with theta = %.100f \n", theta);
		exit(-1);
	}
	int x_min1, x_min2, x_min3, x_min4;
	int x_max1, x_max2, x_max3, x_max4;
	int y_min1, y_min2, y_min3, y_min4;
	int y_max1, y_max2, y_max3, y_max4;
	float largest_particle_weight = FLT_MIN;
	int best_particle = 0;
	double log_max_prior;
	//float scales[N_glint_candidates*N_leds]; replaced with defined constants
	float scales[N_GLINT_CANDIDATES*N_LEDS];
	float scale_thisp;

	x_min1 = std::max(0, int(pupil_center.x - delta_x));
	x_max1 = std::min(Shori, int(pupil_center.x + delta_x));
	y_min1 = std::max(0, int(pupil_center.y - delta_y));
	y_max1 = std::min(Svert, int(pupil_center.y + delta_y));

	if (DO_PERF_T) pt->addTimeStamp("variables");

	// Crop the image around the pupil
	// NOTE: In opencv, Range(a,b) is the same as a:b-1 in Matlab, i.e., an inclusive left boundary and an exclusive right boundary, i.e., [a,b) !
	eyeImage_cropped = eyeImage_diff(cv::Range(y_min1, y_max1), cv::Range(x_min1, x_max1));

	// Filter the image
	cv::filter2D(eyeImage_cropped, eyeImage_filtered, -1, glint_kernel, cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
	cv::minMaxLoc(eyeImage_filtered, &min_val, &max_val, &min_loc, &max_loc);
	im_max = float(max_val);
	//if (0) {imshow("The filtered eye image difference", eyeImage_filtered); waitKey(0); }

	eyeImage_filtered_clone = eyeImage_filtered.clone();
	eyeImage_cropped = eyeImage_filtered.clone();

	//if (0) {imshow("The filtered & cropped eye image difference", eyeImage_cropped); waitKey(0); }

	int Svert_crop = eyeImage_cropped.rows;
	int Shori_crop = eyeImage_cropped.cols;
	//if (0) {cv::imshow("kuva", eyeImage_filtered_clone); cv::waitKey(0); }

	if (DO_PERF_T) pt->addTimeStamp("filters");

	//!! this finds the N_glint_candidates brightest points (with previously found masked by 2xdelta_glint area)
	for (int i = 0; i < N_glint_candidates; i++) {
		cv::minMaxLoc(eyeImage_cropped, &min_val, &max_val, &min_loc, &max_loc);
		glint_candidates[0][i] = max_loc.x + x_min1;
		glint_candidates[1][i] = max_loc.y + y_min1;
		//wipe out the identified glint
		int x0, y0, x1, y1;
		x0 = std::max(float(0), float(max_loc.x - delta_glint));
		x1 = std::min(float(Shori_crop - 1), float(max_loc.x + delta_glint));
		y0 = std::max(float(0), float(max_loc.y - delta_glint));
		y1 = std::min(float(Svert_crop - 1), float(max_loc.y + delta_glint));
		cv::Mat paintover = eyeImage_cropped.colRange(x0, x1).rowRange(y0, y1).getMat(cv::ACCESS_READ); //from UMat to mat for setTo
		paintover.setTo(0);
		/*replaced this:
		eyeImage_cropped(cv::Range(std::max(0, int(max_loc.y-delta_glint)) , std::min(Svert_crop, int(max_loc.y+delta_glint))) , \
		cv::Range(std::max(0, int(max_loc.x-delta_glint)) , std::min(Shori_crop, int(max_loc.x+delta_glint))) ) = \
		eyeImage_cropped(cv::Range(std::max(0, int(max_loc.y-delta_glint)) , std::min(Svert_crop, int(max_loc.y+delta_glint))) , \
		cv::Range(std::max(0, int(max_loc.x-delta_glint)) , std::min(Shori_crop, int(max_loc.x+delta_glint))) ) * 0;
		*/
		loglkh_at_candidates[i] = beta * max_val / im_max;
	}

	if (DO_PERF_T) pt->addTimeStamp("candidates");

	int k = 0;
	for (int j = 0; j < N_glint_candidates; j++) {
		for (int n = 0; n < N_leds; n++) {
			for (int m = 0; m < N_leds; m++) {
				if (m == n) {
					glints_x[j*N_leds + n][m] = glint_candidates[0][k];
					glints_y[j*N_leds + n][m] = glint_candidates[1][k];
					lhoods[j*N_leds + n][m] = loglkh_at_candidates[k];
					maps[j*N_leds + n][m] = loglkh_at_candidates[k];
				}
				else {
					glints_x[j*N_leds + n][m] = 0;
					glints_y[j*N_leds + n][m] = 0;
					lhoods[j*N_leds + n][m] = 0;
					maps[j*N_leds + n][m] = 0;
				}
			}
		}
		k++;
	}

	if (DO_PERF_T) pt->addTimeStamp("k-loop");

	//enumerate ORDERS permutations, such that the sequences 123456, 234561, 345612, ...
	//move outside
	for (int j = 0; j < N_leds; j++) { 
		for (int n = 0; n < N_leds - j; n++) {
			ORDERS[j][n] = n + j;
		}
		for (int m = N_leds - j; m < N_leds; m++) {
			ORDERS[j][m] = m - (N_leds - j);
		}
	}

	if (DO_PERF_T) pt->addTimeStamp("orders");

	int p = 0;
	for (int n = 0; n < N_glint_candidates; n++) {     // loop glint candidates
		for (int j = 0; j < N_leds; j++) {         // loop particles per one glint candidate
			float log_post_MAP = 0;
			double log_max_prior_value = 0;
			eyeImage_aux = eyeImage_filtered_clone.clone();
			for (int k = 1; k < N_leds; k++) { // loop components of one particle  (operate on the same clone from this point on)

				// Insert zeros around the already sampled (i.e., previous) glint:
				x_prev = glints_x[p][ORDERS[j][k - 1]];
				y_prev = glints_y[p][ORDERS[j][k - 1]];
				y_min2 = std::max(0, std::min(Svert_crop - 1, int(y_prev - delta_glint - y_min1)));
				y_max2 = std::max(0, std::min(Svert_crop, int(y_prev + delta_glint - y_min1)));
				x_min2 = std::max(0, std::min(Shori_crop - 1, int(x_prev - delta_glint - x_min1)));
				x_max2 = std::max(0, std::min(Shori_crop, int(x_prev + delta_glint - x_min1)));

				eyeImage_aux(cv::Range(y_min2, y_max2), cv::Range(x_min2, x_max2)).setTo(0);// = eyeImage_aux(cv::Range(y_min2 , y_max2) , cv::Range(x_min2 , x_max2)) * 0;

				// Compute the mean of the glints, matched so far
				float *glints_x_sofar = makeDynArray<float>(k);
				float *glints_y_sofar = makeDynArray<float>(k);
				float *MU_X_sofar = makeDynArray<float>(k);
				float *MU_Y_sofar = makeDynArray<float>(k);
				//not like this: float glints_x_sofar[k], glints_y_sofar[k],  MU_X_sofar[k],  MU_Y_sofar[k],
				float glints_x_sofar_mean = 0, glints_y_sofar_mean = 0, MU_X_sofar_mean = 0, MU_Y_sofar_mean = 0;
				for (int i = 0; i < k; i++) {
					glints_x_sofar[i] = glints_x[p][ORDERS[j][i]];
					glints_y_sofar[i] = glints_y[p][ORDERS[j][i]];
					glints_x_sofar_mean += glints_x_sofar[i];
					glints_y_sofar_mean += glints_y_sofar[i];
					MU_X_sofar[i] = MU_X[ORDERS[j][i]];
					MU_Y_sofar[i] = MU_Y[ORDERS[j][i]];
					MU_X_sofar_mean += MU_X_sofar[i];
					MU_Y_sofar_mean += MU_Y_sofar[i];
				}
				glints_x_sofar_mean = glints_x_sofar_mean / k;
				glints_y_sofar_mean = glints_y_sofar_mean / k;
				MU_X_sofar_mean = MU_X_sofar_mean / k;
				MU_Y_sofar_mean = MU_Y_sofar_mean / k;

				float glints_x_sofar_var = 0, glints_y_sofar_var = 0, MU_X_sofar_var = 0, MU_Y_sofar_var = 0;//, scale;
				if (k > 1) {
					for (int i = 0; i < k; i++) {
						glints_x_sofar_var += (glints_x_sofar[i] - glints_x_sofar_mean) * (glints_x_sofar[i] - glints_x_sofar_mean);
						glints_y_sofar_var += (glints_y_sofar[i] - glints_y_sofar_mean) * (glints_y_sofar[i] - glints_y_sofar_mean);
						MU_X_sofar_var += (MU_X_sofar[i] - MU_X_sofar_mean) * (MU_X_sofar[i] - MU_X_sofar_mean);
						MU_Y_sofar_var += (MU_Y_sofar[i] - MU_Y_sofar_mean) * (MU_Y_sofar[i] - MU_Y_sofar_mean);
					}
					scale_thisp = std::sqrt(glints_x_sofar_var + glints_y_sofar_var) / std::sqrt(MU_X_sofar_var + MU_Y_sofar_var);
					//!! explain the line above and below?
					if (k == N_leds - 1) {
						scales[p] = scale_thisp;
					}
				}
				else { scale_thisp = scale; }

				// Compute the mean without covariance (= "dummy mean"):
				mu_dum.at<float>(0, 0) = glints_x_sofar_mean + (MU_X[ORDERS[j][k]] - MU_X_sofar_mean) * (scale_thisp);
				mu_dum.at<float>(1, 0) = glints_y_sofar_mean + (MU_Y[ORDERS[j][k]] - MU_Y_sofar_mean) * (scale_thisp);

				// Compute the conditional error:
				cv::Mat error_cond(2 * k, 1, CV_32F);
				for (int i = 0; i < k; i++) {
					error_cond.at<float>(i, 0) = glints_x_sofar[i] - glints_x_sofar_mean - (MU_X_sofar[i] - MU_X_sofar_mean) * (scale_thisp);
					error_cond.at<float>(k + i, 0) = glints_y_sofar[i] - glints_y_sofar_mean - (MU_Y_sofar[i] - MU_Y_sofar_mean) * (scale_thisp);
				}

				//delete the dynamical arrays
				delDynArray(glints_x_sofar);
				delDynArray(glints_y_sofar);
				delDynArray(MU_X_sofar);
				delDynArray(MU_Y_sofar);

				if (DO_PERF_T) pt->addTimeStamp("MU\'s");

				// Compute the four blocks of the CM matrix
				cv::Mat CM_topLeft(2, 2, CV_32F);
				CM_topLeft.at<float>(0, 0) = CM.at<float>(ORDERS[j][k], ORDERS[j][k]);
				CM_topLeft.at<float>(0, 1) = CM.at<float>(ORDERS[j][k], ORDERS[j][k] + N_leds);
				CM_topLeft.at<float>(1, 0) = CM.at<float>(ORDERS[j][k] + N_leds, ORDERS[j][k]);
				CM_topLeft.at<float>(1, 1) = CM.at<float>(ORDERS[j][k] + N_leds, ORDERS[j][k] + N_leds);

				cv::Mat CM_topRight(2, k * 2, CV_32F);
				for (int i = 0; i < k; i++)  {
					CM_topRight.at<float>(0, i) = CM.at<float>(ORDERS[j][k], ORDERS[j][i]);
					CM_topRight.at<float>(0, k + i) = CM.at<float>(ORDERS[j][k], ORDERS[j][i] + N_leds);
					CM_topRight.at<float>(1, i) = CM.at<float>(ORDERS[j][k] + N_leds, ORDERS[j][i]);
					CM_topRight.at<float>(1, k + i) = CM.at<float>(ORDERS[j][k] + N_leds, ORDERS[j][i] + N_leds);
				}

				cv::Mat CM_bottomRight(k * 2, k * 2, CV_32F);
				for (int i1 = 0; i1 < k; i1++)  {
					for (int i2 = 0; i2 < k; i2++)  {
						CM_bottomRight.at<float>(i1, i2) = CM.at<float>(ORDERS[j][i1], ORDERS[j][i2]);
						CM_bottomRight.at<float>(i1, k + i2) = CM.at<float>(ORDERS[j][i1], ORDERS[j][i2] + N_leds);
						CM_bottomRight.at<float>(k + i1, i2) = CM.at<float>(ORDERS[j][i1] + N_leds, ORDERS[j][i2]);
						CM_bottomRight.at<float>(k + i1, k + i2) = CM.at<float>(ORDERS[j][i1] + N_leds, ORDERS[j][i2] + N_leds);
					}
				}

				cv::Mat invCM_bottomRight(k * 2, k * 2, CV_32F);
				if (invert(CM_bottomRight, invCM_bottomRight, cv::DECOMP_CHOLESKY) == 0) {
					// printf("getGlints.cpp: The bottom right block of CM could not be inverted [n=%d, j=%d, k=%d] \n", n,j,k); exit(-1);
					invCM_bottomRight = cv::Mat::eye(CM_bottomRight.size(), CM_bottomRight.type());
				}

				cv::Mat CM_bottomLeft(k * 2, k * 2, CV_32F);
				CM_bottomLeft = CM_topRight.t();  // Covariance matrix is always symmetric

				if (DO_PERF_T) pt->addTimeStamp("covariancematrix");

				// Compute the covariance-corrected conditional mean
				mu_cond = mu_dum + CM_topRight * invCM_bottomRight * error_cond;
				if (mu_cond.at<float>(0, 0) > Shori - 1) { mu_cond.at<float>(0, 0) = float(Shori); }   // Let's force mu inside the image, if it's not
				if (mu_cond.at<float>(1, 0) > Svert - 1) { mu_cond.at<float>(1, 0) = float(Svert); }

				// Compute the covariance-corrected conditional covariance
				C_cond = CM_topLeft - CM_topRight * invCM_bottomRight * CM_bottomLeft;

				// Scale the covariance
				C_cond = C_cond * scale_thisp*scale_thisp;//(*scale)*(*scale);

				// Regularize the covariance (add elements to all matrix elements or only to diagonal elements?)
				C_cond.at<float>(0, 0) = C_cond.at<float>(0, 0) + reg_coef*k;
				C_cond.at<float>(1, 1) = C_cond.at<float>(1, 1) + reg_coef*k;

				cv::Mat invC_cond(2, 2, CV_32F);
				if (cv::invert(C_cond, invC_cond, cv::DECOMP_CHOLESKY) == 0) {
					printf("getGlints.cpp: C_conditional could not be inverted \n");
					invC_cond = cv::Mat::eye(2, 2, CV_32F);
				}
				cv::Mat C_comb(2, 2, CV_32F);
				if (invert(invC_cond + invC_dyn, C_comb, cv::DECOMP_CHOLESKY) == 0) {
					printf("getGlints.cpp: C_combined could not be inverted \n");
					C_comb = cv::Mat::eye(2, 2, CV_32F);
				}

				if (DO_PERF_T) pt->addTimeStamp("cv inverts");

				// Form the likelihood (define the crop area first)
				x_min3 = int(round(mu_cond.at<float>(0, 0) - 3 * C_comb.at<float>(0, 0) - delta_extra));
				x_max3 = int(round(mu_cond.at<float>(0, 0) + 3 * C_comb.at<float>(0, 0) + delta_extra));
				y_min3 = int(round(mu_cond.at<float>(1, 0) - 3 * C_comb.at<float>(1, 1) - delta_extra));
				y_max3 = int(round(mu_cond.at<float>(1, 0) + 3 * C_comb.at<float>(1, 1) + delta_extra));
				x_max3 = std::max(x_min3 + 1, x_max3);
				y_max3 = std::max(y_min3 + 1, y_max3);

				x_min4 = x_min3 - x_min1; // int(round(mu_cond.at<float>(0,0)-x_min1 - 3*C_comb.at<float>(0,0) - delta_extra));
				x_min3 = int(x_min3 - (x_min4 < 0)*x_min4 - (x_min4 >= Shori_crop)*(x_min4 - Shori_crop + 1));
				x_min4 = int(std::max(0, std::min(Shori_crop - 1, x_min4)));
				x_max4 = x_max3 - x_min1; //int(round(mu_cond.at<float>(0,0)-x_min1 + 3*C_comb.at<float>(0,0) + delta_extra));
				x_max3 = int(x_max3 - (x_max4 <= 0)*(x_max4 - 1) - (x_max4 >= Shori_crop)*(x_max4 - Shori_crop));
				x_max4 = int(std::max(1, std::min(Shori_crop, x_max4)));

				y_min4 = y_min3 - y_min1; // int(round(mu_cond.at<float>(1,0)-y_min1 - 3*C_comb.at<float>(1,1) - delta_extra));
				y_min3 = int(y_min3 - (y_min4 < 0)*y_min4 - (y_min4 >= Svert_crop)*(y_min4 - Svert_crop + 1));
				y_min4 = int(std::max(0, std::min(Svert_crop - 1, y_min4)));
				y_max4 = y_max3 - y_min1; // int(round(mu_cond.at<float>(1,0)-y_min1 + 3*C_comb.at<float>(1,1) + delta_extra));
				y_max3 = int(y_max3 - (y_max4 <= 0)*(y_max4 - 1) - (y_max4 >= Svert_crop)*(y_max4 - Svert_crop));
				y_max4 = int(std::max(1, std::min(Svert_crop, y_max4)));

				eyeImage_aux_crop = eyeImage_aux(cv::Range(y_min4, y_max4), cv::Range(x_min4, x_max4));

				cv::Mat eyeImage_aux_crop_float; //(eyeImage_aux_crop.rows, eyeImage_aux_crop.cols, CV_32F);
				eyeImage_aux_crop.convertTo(eyeImage_aux_crop_float, CV_32F);
				cv::Mat log_lhood = beta * eyeImage_aux_crop_float / im_max;
				
				if (DO_PERF_T) pt->addTimeStamp("likelihood");

				// Form the conditional prior distribution
				cv::Mat log_prior(y_max3 - y_min3, x_max3 - x_min3, CV_32F);
				// Form the coordinates for which to evaluate the mvnpdf
				cv::Mat xv(1, x_max3 - x_min3, CV_32F);
				cv::Mat yv(y_max3 - y_min3, 1, CV_32F);
				int ii = 0; for (int x = x_min3; x < x_max3; x++) { xv.at<float>(0, ii++) = x; }
				ii = 0;     for (int y = y_min3; y < y_max3; y++) { yv.at<float>(ii++, 0) = y; }
				cv::Mat xx = repeat(xv, yv.total(), 1);
				cv::Mat yy = repeat(yv, 1, xv.total());
				cv::Mat coords(2, xx.total(), CV_32F);
				cv::Mat xCol(coords(cv::Range(0, 1), cv::Range::all()));
				(xx.reshape(0, 1)).copyTo(xCol);
				cv::Mat yCol(coords(cv::Range(1, 2), cv::Range::all()));
				(yy.reshape(0, 1)).copyTo(yCol);

				if (DO_PERF_T) pt->addTimeStamp("conditional prior");

				if (theta < 0)  {  // Use only the conditional prior
					log_prior = log_mvnpdf(coords, repeat(mu_cond, 1, coords.cols), C_cond).reshape(0, xx.rows);

				}
				else {  // Combine the conditional and dynamical prior
					// Form the product of two mvnpdf's; see, e.g., http://compbio.fmph.uniba.sk/vyuka/ml/old/2008/handouts/matrix-cookbook.pdf

					mu_dyn.at<float>(0, 0) = float(glintPoints_prev[ORDERS[j][k]].x);
					mu_dyn.at<float>(1, 0) = float(glintPoints_prev[ORDERS[j][k]].y);
					cv::Mat log_scaling_factor = log_mvnpdf(mu_cond, mu_dyn, C_cond + C_dyn);  // This is actually a scalar
					log_prior = log_scaling_factor.at<float>(0, 0) + log_mvnpdf(coords, repeat(C_comb * (invC_cond*mu_cond + invC_dyn*mu_dyn), 1, coords.cols), C_comb).reshape(0, xx.rows);
					float log_norm_factor = -0.5*(2 * log(2 * PI) + log(determinant(C_cond + C_dyn)));
					log_max_prior_value = log_max_prior_value + double(log_scaling_factor.at<float>(0, 0)) + log_norm_factor;  // maximum value of the (combined) prior distribution
				}

				//!! H�T�FIKSI - T�m� aiheuttaa systemaattisen kaatumisen alussa?
/*				bool bad = std::isnan(log_prior.at<float>(0,0)) || std::isnan(log_prior.at<float>(0,1)) || std::isnan(log_prior.at<float>(1,0)) || std::isnan(log_prior.at<float>(1,1));
				if (bad) {
					log_prior = log_lhood * 0;
				}
*/

				// Compute the logarithm of posterior distribution
				cv::Mat log_post = log_prior + log_lhood;

				// Posterior with the occlusion model (skip this)
				/*if (0) {
				Mat lhood; cv::exp(log_lhood, lhood);
				double lkh_notv = exp(beta*0.5);
				Mat log_lhood2; cv::log(lhood + lkh_notv, log_lhood2);
				log_post = log_prior + log_lhood2;
				}
				*/

				// The glint location shall be the MAP estimate
				minMaxLoc(log_post, &min_val, &max_val, &min_loc, &max_loc);
				glints_x[p][ORDERS[j][k]] = max_loc.x + x_min3;// + FAST_WAY*x_min1;
				glints_y[p][ORDERS[j][k]] = max_loc.y + y_min3;// + FAST_WAY*y_min1;
				log_post_MAP = log_post_MAP + max_val;

				lhoods[p][ORDERS[j][k]] = log_lhood.at<float>(max_loc);	// Get also the likelihood values at MAP estimates
				maps[p][ORDERS[j][k]] = max_val;     // Get also the MAP values

			}  // end of k loop

			// Find the winner particle (i.e., with largest weight)
			if (log_post_MAP > largest_particle_weight) {
				best_particle = p;
				largest_particle_weight = log_post_MAP;
				log_max_prior = log_max_prior_value;
			}
			p++;  // increase the index of particle

		}  // end of j loop
	}  // end of n loop

	if (DO_PERF_T) pt->addTimeStamp("winner");

	// A heuristic score for the match: the log MAP devided by the highest possible log post ---> scaled to [0,1]
	//!! use min
	score = std::min(1.0, largest_particle_weight / (N_leds * beta + log_max_prior));
	//score = largest_particle_weight / (N_leds * beta + log_max_prior);
	//if (score > 1) {  // double check...
	//	score = 1;
	//}

	for (int i = 0; i < N_leds; i++) {
		loglhoods[i] = lhoods[best_particle][i] / beta;  // Return the scaled log likelihood values (i.e., glint intensities scaled between [0,1])
		//loglhoods[i] = maps[best_particle][i];  // Return the MAP values (i.e., glint intensities scaled between [0,1])
	}

	// Return the particle with the largest weight
	//cv::Point2d glintPoint;
	std::vector<cv::Point2d> glintPoints;
	for (int i = 0; i < N_leds; i++) {
		//glintPoint.x = glints_x[best_particle][i];
		//glintPoint.y = glints_y[best_particle][i];
		//glintPoints.push_back(glintPoint);
		glintPoints.push_back(cv::Point2d(glints_x[best_particle][i], glints_y[best_particle][i]));
	}

	scale = scales[best_particle];

	framecounter++;

	if (DO_PERF_T) pt->addTimeStamp("the rest");

	if (bUpdateGlintModel) updateGlintModel(glintPoints, score, 10);//scale, 10);

	if (DO_PERF_T) pt->addTimeStamp("update glint model");

	//if(DO_PERF_T) pt->dumpTimeStamps(std::cout);
	if (DO_PERF_T) pt->clearTimeStamps();

	return glintPoints;
}

void GlintFinder::updateGlintModel(
	std::vector<cv::Point2d> glintPoints,
	//	float MU_X[],
	//	float MU_Y[],
	//	cv::Mat &CM,
	const double score,
	const int N_prior_meas)
{

	//TODO: move this to settings->read from config
	int N_leds = 6;

	scale = score / (framecounter + N_prior_meas) * scale + (framecounter + N_prior_meas - score) / (framecounter + N_prior_meas) * scale;  // Update scale

	// Update the mean and covariance recursively
	float MU_X_meas, MU_Y_meas;
	float glintPoints_X_mean = 0, glintPoints_Y_mean = 0;
	cv::Mat mu_xy = cv::Mat(2 * N_leds, 1, CV_32F);
	cv::Mat mu_xy_meas = cv::Mat(2 * N_leds, 1, CV_32F);
	for (int i = 0; i < N_leds; i++) {
		glintPoints_X_mean = glintPoints_X_mean + 1.0 / N_leds*glintPoints[i].x;
		glintPoints_Y_mean = glintPoints_Y_mean + 1.0 / N_leds*glintPoints[i].y;
	}
	for (int i = 0; i < N_leds; i++) {
		MU_X_meas = (glintPoints[i].x - glintPoints_X_mean) / scale;
		MU_Y_meas = (glintPoints[i].y - glintPoints_Y_mean) / scale;
		mu_xy.at<float>(2 * i, 0) = MU_X[i];  // Copy the previous mean values (for computing the covariance matrix)
		mu_xy.at<float>(2 * i + 1, 0) = MU_Y[i];
		MU_X[i] = score / (framecounter + N_prior_meas) * MU_X_meas + (framecounter + N_prior_meas - score) / (framecounter + N_prior_meas) * MU_X[i];  // Update mean
		MU_Y[i] = score / (framecounter + N_prior_meas) * MU_Y_meas + (framecounter + N_prior_meas - score) / (framecounter + N_prior_meas) * MU_Y[i];
		mu_xy_meas.at<float>(2 * i, 0) = MU_X_meas;  // Current measurements (for computing the covariance matrix)
		mu_xy_meas.at<float>(2 * i + 1, 0) = MU_Y_meas;
	}
	// Recursive estimate of the covariance matrix (http://lmb.informatik.uni-freiburg.de/lectures/mustererkennung/Englische_Folien/07_c_ME_en.pdf)
	CM = (framecounter + N_prior_meas - score) / (framecounter + N_prior_meas) * (CM + score / (framecounter + N_prior_meas) * (mu_xy - mu_xy_meas) * (mu_xy - mu_xy_meas).t());
}

