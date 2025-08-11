% MATLAB script to visualize SLAM trajectory and point cloud in 3D
% Point cloud is in map frame; trajectory provides Tm2b (map-to-body transform)
clear all; close all; clc;

% --- File Paths ---
% Replace with actual paths to your trajectory and point cloud files
traj_file = 'trajectory_20250811_160505.txt'; % Example filename
pc_file = 'map_20250811_160505.txt';          % Example filename

% --- Read Point Cloud ---
% Format: x y z (map frame coordinates)
point_cloud = load(pc_file); % Reads into Nx3 matrix [x, y, z]
fprintf('Loaded %d points from point cloud file (map frame).\n', size(point_cloud, 1));

% --- Read Trajectory ---
% Format: timestamp T(0,0) T(0,1) T(0,2) T(0,3) ... T(3,3) w(0) ... w(5)
% Tm2b: 4x4 transform from map to body frame
traj_data = load(traj_file);
n_traj = size(traj_data, 1);
fprintf('Loaded %d trajectory points.\n', n_traj);

% Extract positions (Tm2b(1:3,4)) and rotations (Tm2b(1:3,1:3)) in map frame
positions = zeros(n_traj, 3); % [x, y, z] in map frame
rotations = zeros(3, 3, n_traj); % Rotation matrices (map to body)
timestamps = traj_data(:, 1) * 1e-9; % Convert nanoseconds to seconds

for i = 1:n_traj
    % Extract 4x4 Tm2b (map-to-body transform, columns 2:17)
    Tm2b = reshape(traj_data(i, 2:17), [4, 4])';
    positions(i, :) = Tm2b(1:3, 4)'; % Body position in map frame
    rotations(:, :, i) = Tm2b(1:3, 1:3); % Rotation: map to body
    
    % Validate rotation matrix (should be orthogonal, det ≈ 1)
    R = rotations(:, :, i);
    if norm(R' * R - eye(3), 'fro') > 1e-6 || abs(det(R) - 1) > 1e-6
        warning('Invalid rotation matrix at index %d: not orthogonal or det ≠ 1', i);
    end
end

% --- Create 3D Plot ---
figure('Name', 'SLAM Trajectory and Point Cloud (Map Frame)', 'Position', [1200, 1200, 1200, 1200]);
hold on;

% Plot point cloud (map frame, downsample if too dense)
pc_plot = point_cloud;

scatter3(pc_plot(:, 1), pc_plot(:, 2), pc_plot(:, 3), 2, 'b.', 'MarkerFaceAlpha', 0.3, ...
    'DisplayName', 'Point Cloud (Map Frame)');
fprintf('Plotted %d points.\n', size(pc_plot, 1));

% Plot trajectory as a line (positions in map frame)
plot3(positions(:, 1), positions(:, 2), positions(:, 3), 'r-', 'LineWidth', 2, ...
    'DisplayName', 'Trajectory (Map Frame)');

% --- Plot Vehicle Axes in Map Frame ---
% Tm2b rotation maps map-frame vectors to body frame, so apply R to body-frame unit vectors
sample_interval = max(1, floor(n_traj / 20)); % Show ~20 sets of axes
axis_length = 0.5; % Length of vehicle axes in meters (adjust as needed)
for i = 1:sample_interval:n_traj
    R = rotations(:, :, i); % Rotation: map to body
    pos = positions(i, :); % Position in map frame
    % Body-frame unit vectors (X, Y, Z)
    x_body = [1; 0; 0]; y_body = [0; 1; 0]; z_body = [0; 0; 1];
    % Transform to map frame: v_map = R' * v_body (since R is map-to-body)
    x_map = R' * x_body * axis_length;
    y_map = R' * y_body * axis_length;
    z_map = R' * z_body * axis_length;
    % Plot axes: X (red), Y (green), Z (blue)
    quiver3(pos(1), pos(2), pos(3), x_map(1), x_map(2), x_map(3), ...
        'r', 'LineWidth', 1.5, 'MaxHeadSize', 0.5, 'AutoScale', 'off', 'DisplayName', 'X-Axis');
    quiver3(pos(1), pos(2), pos(3), y_map(1), y_map(2), y_map(3), ...
        'g', 'LineWidth', 1.5, 'MaxHeadSize', 0.5, 'AutoScale', 'off', 'DisplayName', 'Y-Axis');
    quiver3(pos(1), pos(2), pos(3), z_map(1), z_map(2), z_map(3), ...
        'b', 'LineWidth', 1.5, 'MaxHeadSize', 0.5, 'AutoScale', 'off', 'DisplayName', 'Z-Axis');
end

% --- Optional: Annotate Trajectory Points ---
% Add timestamp labels for first, middle, and last points
label_indices = [1, floor(n_traj/2), n_traj];
for i = label_indices
    text(positions(i, 1), positions(i, 2), positions(i, 3), ...
        sprintf('t=%.3fs', timestamps(i) - timestamps(1)), ...
        'FontSize', 8, 'Color', 'k', 'VerticalAlignment', 'bottom');
end

% --- Plot Settings ---
grid on;
axis equal; % Equal scaling for all axes
xlabel('X (m)');
ylabel('Y (m)');
zlabel('Z (m)');
title('SLAM Trajectory and Point Cloud (Map Frame)');
legend('Point Cloud', 'Trajectory', 'X-Axis (Body)', 'Y-Axis (Body)', 'Z-Axis (Body)', 'Location', 'best');
view(3); % 3D view
hold off;

% --- Optional: Save Plot ---
% Uncomment to save the figure as a PNG
% saveas(gcf, 'slam_visualization.png');