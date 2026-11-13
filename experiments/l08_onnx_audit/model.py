import numpy as np
import torch
from torch import nn

SEED = 42
SHAPE = (1, 1, 32, 32)


class TinyDocNet(nn.Module):
    def __init__(self):
        super().__init__()
        self.conv1 = nn.Conv2d(1, 8, 3, padding=1)
        self.bn1 = nn.BatchNorm2d(8)
        self.relu1 = nn.ReLU()
        self.pool = nn.AvgPool2d(2)
        self.conv2 = nn.Conv2d(8, 16, 3, padding=1)
        self.relu2 = nn.ReLU()
        self.global_pool = nn.AdaptiveAvgPool2d(1)
        self.flatten = nn.Flatten()
        self.fc = nn.Linear(16, 3)

    def forward(self, x):
        x = self.pool(self.relu1(self.bn1(self.conv1(x))))
        return self.fc(self.flatten(self.global_pool(self.relu2(self.conv2(x)))))


def make_model():
    with torch.random.fork_rng(devices=[]):
        torch.manual_seed(SEED)
        model = TinyDocNet()
    with torch.no_grad():
        model.bn1.weight.copy_(torch.linspace(0.6, 1.4, 8))
        model.bn1.bias.copy_(torch.linspace(-0.2, 0.15, 8))
        model.bn1.running_mean.copy_(torch.linspace(-0.3, 0.25, 8))
        model.bn1.running_var.copy_(torch.linspace(0.5, 1.8, 8))
    return model.eval()


def make_inputs():
    rng = np.random.default_rng(SEED)
    pulse = np.zeros(SHAPE, dtype=np.float32)
    pulse[0, 0, 15, 16] = 1
    checker = (np.indices((32, 32)).sum(axis=0) % 2).astype(np.float32).reshape(SHAPE)
    return {"zeros": np.zeros(SHAPE, np.float32), "ones": np.ones(SHAPE, np.float32),
            "random": rng.random(SHAPE, dtype=np.float32), "pulse": pulse,
            "checker": checker, "negative_stress": rng.uniform(-1, 1, SHAPE).astype(np.float32)}


def validate_input(x):
    if x.shape != SHAPE or x.dtype != np.float32 or not np.isfinite(x).all():
        raise ValueError('expected finite float32 [1,1,32,32]')
